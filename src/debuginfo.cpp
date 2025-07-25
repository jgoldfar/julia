// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "platform.h"

#include "llvm-version.h"
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Object/SymbolSize.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/IR/Function.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Mangler.h>
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/BinaryFormat/Magic.h>
#include <llvm/Object/MachO.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm-Compression.h>
#include <llvm/Support/Compression.h>

#ifdef _OS_DARWIN_
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace llvm;

#include "jitlayers.h"
#include "debuginfo.h"
#if defined(_OS_LINUX_)
#  include <link.h>
#endif
#include "processor.h"

#include <string>
#include <map>
#include <vector>
#include <set>
#include <mutex>
#include "julia_assert.h"
#include "debug-registry.h"

static JITDebugInfoRegistry *DebugRegistry = new JITDebugInfoRegistry;

static JITDebugInfoRegistry &getJITDebugRegistry() JL_NOTSAFEPOINT {
    return *DebugRegistry;
}

struct debug_link_info {
    StringRef filename;
    uint32_t crc32;
};

#if (defined(_OS_LINUX_) || defined(_OS_FREEBSD_) || (defined(_OS_DARWIN_) && defined(LLVM_SHLIB)))
extern "C" void __register_frame(void*) JL_NOTSAFEPOINT;
extern "C" void __deregister_frame(void*) JL_NOTSAFEPOINT;

template <typename callback>
static void processFDEs(const char *EHFrameAddr, size_t EHFrameSize, callback f) JL_NOTSAFEPOINT
{
    const char *P = EHFrameAddr;
    const char *End = P + EHFrameSize;
    do {
        const char *Entry = P;
        P += 4;
        assert(P <= End);
        uint32_t Length = *(const uint32_t*)Entry;
        // Length == 0: Terminator
        if (Length == 0)
            break;
        assert(P + Length <= End);
        uint32_t Offset = *(const uint32_t*)P;
        // Offset == 0: CIE
        if (Offset != 0)
            f(Entry);
        P += Length;
    } while (P != End);
}
#endif

std::string JITDebugInfoRegistry::mangle(StringRef Name, const DataLayout &DL)
{
    std::string MangledName;
    {
        raw_string_ostream MangledNameStream(MangledName);
        Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
    }
    return MangledName;
}

void JITDebugInfoRegistry::add_code_in_flight(StringRef name, jl_code_instance_t *codeinst, const DataLayout &DL) {
    (**codeinst_in_flight)[mangle(name, DL)] = codeinst;
}

jl_code_instance_t *JITDebugInfoRegistry::lookupCodeInstance(size_t pointer)
{
    jl_lock_profile();
    auto region = cimap.lower_bound(pointer);
    jl_code_instance_t *linfo = NULL;
    if (region != cimap.end() && pointer < region->first + region->second.first)
        linfo = region->second.second;
    jl_unlock_profile();
    return linfo;
}

//Protected by debuginfo_asyncsafe (profile) lock
JITDebugInfoRegistry::objectmap_t &
JITDebugInfoRegistry::getObjectMap()
{
    return objectmap;
}

void JITDebugInfoRegistry::add_image_info(image_info_t info) {
    (**this->image_info)[info.base] = info;
}


bool JITDebugInfoRegistry::get_image_info(uint64_t base, JITDebugInfoRegistry::image_info_t *info) const {
    auto infos = *this->image_info;
    auto it = infos->find(base);
    if (it != infos->end()) {
        *info = it->second;
        return true;
    }
    return false;
}

JITDebugInfoRegistry::Locked<JITDebugInfoRegistry::objfilemap_t>::LockT
JITDebugInfoRegistry::get_objfile_map() {
    return *this->objfilemap;
}

JITDebugInfoRegistry::JITDebugInfoRegistry() { }

struct unw_table_entry
{
    int32_t start_ip_offset;
    int32_t fde_offset;
};

// some actions aren't signal (especially profiler) safe so we acquire a lock
// around them to establish a mutual exclusion with unwinding from a signal
template <typename T>
static void jl_profile_atomic(T f) JL_NOTSAFEPOINT
{
    int havelock = jl_lock_profile_wr();
    assert(havelock);
#ifndef _OS_WINDOWS_
    sigset_t sset;
    sigset_t oset;
    sigfillset(&sset);
    pthread_sigmask(SIG_BLOCK, &sset, &oset);
#endif
    f();
#ifndef _OS_WINDOWS_
    pthread_sigmask(SIG_SETMASK, &oset, NULL);
#endif
    if (havelock)
        jl_unlock_profile_wr();
}


// --- storing and accessing source location metadata ---
void jl_add_code_in_flight(StringRef name, jl_code_instance_t *codeinst, const DataLayout &DL) JL_NOTSAFEPOINT_LEAVE JL_NOTSAFEPOINT_ENTER
{
    // Non-opaque-closure MethodInstances are considered globally rooted
    // through their methods, but for OC, we need to create a global root
    // here.
    jl_method_instance_t *mi = jl_get_ci_mi(codeinst);
    if (jl_is_method(mi->def.value) && mi->def.method->is_for_opaque_closure) {
        jl_task_t *ct = jl_current_task;
        int8_t gc_state = jl_gc_unsafe_enter(ct->ptls);
        jl_as_global_root((jl_value_t*)mi, 1);
        jl_gc_unsafe_leave(ct->ptls, gc_state);
    }
    getJITDebugRegistry().add_code_in_flight(name, codeinst, DL);
}


#if defined(_OS_WINDOWS_)
static void create_PRUNTIME_FUNCTION(uint8_t *Code, size_t Size, StringRef fnname,
                                     uint8_t *Section, size_t Allocated, uint8_t *UnwindData)
{
    // GC safe
    DWORD mod_size = 0;
#if defined(_CPU_X86_64_)
    PRUNTIME_FUNCTION tbl = (PRUNTIME_FUNCTION)malloc_s(sizeof(RUNTIME_FUNCTION));
    tbl->BeginAddress = (DWORD)(Code - Section);
    tbl->EndAddress = (DWORD)(Code - Section + Size);
    tbl->UnwindData = (DWORD)(UnwindData - Section);
    assert(Code >= Section && Code + Size <= Section + Allocated);
    assert(UnwindData >= Section && UnwindData <= Section + Allocated);
#else // defined(_CPU_X86_64_)
    Section += (uintptr_t)Code;
    mod_size = Size;
#endif
    if (0) {
        uv_mutex_lock(&jl_in_stackwalk);
        if (mod_size && !SymLoadModuleEx(GetCurrentProcess(), NULL, NULL, NULL, (DWORD64)Section, mod_size, NULL, SLMFLAG_VIRTUAL)) {
            static int warned = 0;
            if (!warned) {
                jl_safe_printf("WARNING: failed to insert module info for backtrace: %lu\n", GetLastError());
                warned = 1;
            }
        }
        else {
            size_t len = fnname.size()+1;
            if (len > MAX_SYM_NAME)
                len = MAX_SYM_NAME;
            char *name = (char*)alloca(len);
            memcpy(name, fnname.data(), len-1);
            name[len-1] = 0;
            if (!SymAddSymbol(GetCurrentProcess(), (ULONG64)Section, name,
                        (DWORD64)Code, (DWORD)Size, 0)) {
                jl_safe_printf("WARNING: failed to insert function name %s into debug info: %lu\n", name, GetLastError());
            }
        }
        uv_mutex_unlock(&jl_in_stackwalk);
    }
#if defined(_CPU_X86_64_)
    jl_profile_atomic([&]() JL_NOTSAFEPOINT {
        if (!RtlAddFunctionTable(tbl, 1, (DWORD64)Section)) {
            static int warned = 0;
            if (!warned) {
                jl_safe_printf("WARNING: failed to insert function stack unwind info: %lu\n", GetLastError());
                warned = 1;
            }
        }
    });
#endif
}
#endif

void JITDebugInfoRegistry::registerJITObject(const object::ObjectFile &Object,
                        std::function<uint64_t(const StringRef &)> getLoadAddress)
{
    object::section_iterator EndSection = Object.section_end();

    bool anyfunctions = false;
    for (const object::SymbolRef &sym_iter : Object.symbols()) {
        object::SymbolRef::Type SymbolType = cantFail(sym_iter.getType());
        if (SymbolType != object::SymbolRef::ST_Function)
            continue;
        anyfunctions = true;
        break;
    }
    if (!anyfunctions)
        return;

#ifdef _CPU_ARM_
    // ARM does not have/use .eh_frame
    uint64_t arm_exidx_addr = 0;
    size_t arm_exidx_len = 0;
    uint64_t arm_text_addr = 0;
    size_t arm_text_len = 0;
    for (auto &section: Object.sections()) {
        bool istext = false;
        if (section.isText()) {
            istext = true;
        }
        else {
            auto sName = section.getName();
            if (!sName)
                continue;
            if (sName.get() != ".ARM.exidx") {
                continue;
            }
        }
        uint64_t loadaddr = getLoadAddress(section.getName().get());
        size_t seclen = section.getSize();
        if (istext) {
            arm_text_addr = loadaddr;
            arm_text_len = seclen;
            if (!arm_exidx_addr) {
                continue;
            }
        }
        else {
            arm_exidx_addr = loadaddr;
            arm_exidx_len = seclen;
            if (!arm_text_addr) {
                continue;
            }
        }
        unw_dyn_info_t *di = new unw_dyn_info_t;
        di->gp = 0;
        di->format = UNW_INFO_FORMAT_ARM_EXIDX;
        di->start_ip = (uintptr_t)arm_text_addr;
        di->end_ip = (uintptr_t)(arm_text_addr + arm_text_len);
        di->u.rti.name_ptr = 0;
        di->u.rti.table_data = arm_exidx_addr;
        di->u.rti.table_len = arm_exidx_len;
        jl_profile_atomic([&]() JL_NOTSAFEPOINT {
            _U_dyn_register(di);
        });
        break;
    }
#endif

#if defined(_OS_WINDOWS_)
    uint64_t SectionAddrCheck = 0;
    uint64_t SectionLoadCheck = 0; (void)SectionLoadCheck;
    uint8_t *UnwindData = NULL;
#if defined(_CPU_X86_64_)
    uint8_t *catchjmp = NULL;
    for (const object::SymbolRef &sym_iter : Object.symbols()) {
        StringRef sName = cantFail(sym_iter.getName());
        if (sName == "__UnwindData" || sName == "__catchjmp") {
            uint64_t Addr = cantFail(sym_iter.getAddress()); // offset into object (including section offset)
            auto Section = cantFail(sym_iter.getSection());
            assert(Section != EndSection && Section->isText());
            uint64_t SectionAddr = Section->getAddress();
            StringRef secName = cantFail(Section->getName());
            uint64_t SectionLoadAddr = getLoadAddress(secName);
            assert(SectionLoadAddr);
            if (SectionAddrCheck) // assert that all of the Sections are at the same location
                assert(SectionAddrCheck == SectionAddr &&
                        SectionLoadCheck == SectionLoadAddr);
            SectionAddrCheck = SectionAddr;
            SectionLoadCheck = SectionLoadAddr;
            Addr += SectionLoadAddr - SectionAddr;
            if (sName == "__UnwindData") {
                UnwindData = (uint8_t*)Addr;
            }
            else if (sName == "__catchjmp") {
                catchjmp = (uint8_t*)Addr;
            }
        }
    }
    assert(catchjmp);
    assert(UnwindData);
    assert(SectionLoadCheck);
#endif // defined(_OS_X86_64_)
#endif // defined(_OS_WINDOWS_)

    SmallVector<uint8_t, 0> packed;
    ArrayRef<uint8_t> unpacked = arrayRefFromStringRef(Object.getData());
    std::optional<compression::Format> F;
    if (compression::zstd::isAvailable())
        F = compression::Format::Zstd;
    else if (compression::zlib::isAvailable())
        F = compression::Format::Zlib;
    if (F)
        compression::compress(*F, unpacked, packed);
    // intentionally leak this so that we don't need to ref-count it
    // intentionally copy the input so that we exact-size the allocation (since no shrink_to_fit function)
    auto ObjectCopy = new LazyObjectInfo{SmallVector<uint8_t, 0>(F ? ArrayRef(packed) : unpacked), F ? Object.getData().size() : 0};
    jl_jit_add_bytes(ObjectCopy->data.size());
    auto symbols = object::computeSymbolSizes(Object);
    bool hassection = false;
    for (const auto &sym_size : symbols) {
        const object::SymbolRef &sym_iter = sym_size.first;
        object::SymbolRef::Type SymbolType = cantFail(sym_iter.getType());
        if (SymbolType != object::SymbolRef::ST_Function) continue;
        uint64_t Addr = cantFail(sym_iter.getAddress());
        auto Section = cantFail(sym_iter.getSection());
        if (Section == EndSection) continue;
        if (!Section->isText()) continue;
        uint64_t SectionAddr = Section->getAddress();
        StringRef secName = cantFail(Section->getName());
        uint64_t SectionLoadAddr = getLoadAddress(secName);
        Addr += SectionLoadAddr - SectionAddr;
        StringRef sName = cantFail(sym_iter.getName());
        uint64_t SectionSize = Section->getSize();
        size_t Size = sym_size.second;
#if defined(_OS_WINDOWS_)
        if (SectionAddrCheck)
            assert(SectionAddrCheck == SectionAddr &&
                    SectionLoadCheck == SectionLoadAddr);
        SectionAddrCheck = SectionAddr;
        SectionLoadCheck = SectionLoadAddr;
        create_PRUNTIME_FUNCTION(
                (uint8_t*)(uintptr_t)Addr, (size_t)Size, sName,
                (uint8_t*)(uintptr_t)SectionLoadAddr, (size_t)SectionSize, UnwindData);
#endif
        jl_code_instance_t *codeinst = NULL;
        {
            auto lock = *this->codeinst_in_flight;
            auto &codeinst_in_flight = *lock;
            StringMap<jl_code_instance_t*>::iterator codeinst_it = codeinst_in_flight.find(sName);
            if (codeinst_it != codeinst_in_flight.end()) {
                codeinst = codeinst_it->second;
                codeinst_in_flight.erase(codeinst_it);
            }
        }
        jl_profile_atomic([&]() JL_NOTSAFEPOINT {
            if (codeinst)
                cimap[Addr] = std::make_pair(Size, codeinst);
            hassection = true;
            objectmap.insert(std::pair{SectionLoadAddr, SectionInfo{
                ObjectCopy,
                (size_t)SectionSize,
                (ptrdiff_t)(SectionAddr - SectionLoadAddr),
                Section->getIndex()
                }});
        });
    }
    if (!hassection) // clang-sa demands that we do this to fool cplusplus.NewDeleteLeaks
        delete ObjectCopy;
}

void jl_register_jit_object(const object::ObjectFile &Object,
                            std::function<uint64_t(const StringRef &)> getLoadAddress)
{
    getJITDebugRegistry().registerJITObject(Object, getLoadAddress);
}

// TODO: convert the safe names from aotcomile.cpp:makeSafeName back into symbols
static std::pair<char *, bool> jl_demangle(const char *name) JL_NOTSAFEPOINT
{
    // This function is not allowed to reference any TLS variables since
    // it can be called from an unmanaged thread on OSX.
    const char *start = name + 6;
    const char *end = name + strlen(name);
    char *ret;
    if (end <= start)
        goto done;
    if (strncmp(name, "japi1_", 6) &&
        strncmp(name, "japi3_", 6) &&
        strncmp(name, "julia_", 6) &&
        strncmp(name, "jsys1_", 6) &&
        strncmp(name, "jlsys_", 6))
        goto done;
    if (*start == '\0')
        goto done;
    while (*(--end) != '_') {
        char c = *end;
        if (c < '0' || c > '9')
            goto done;
    }
    if (end <= start)
        goto done;
    ret = (char*)malloc_s(end - start + 1);
    memcpy(ret, start, end - start);
    ret[end - start] = '\0';
    return std::make_pair(ret, true);
done:
    return std::make_pair(strdup(name), false);
}

// *frames is a one element array containing whatever we could come up
// with for the current frame. here we'll try to expand it using debug info
// func_name and file_name are either NULL or malloc'd pointers
static int lookup_pointer(
        object::SectionRef Section, DIContext *context,
        jl_frame_t **frames, size_t pointer, int64_t slide,
        bool demangle, bool noInline) JL_NOTSAFEPOINT
{
    // This function is not allowed to reference any TLS variables
    // since it can be called from an unmanaged thread on OSX.
    if (!context || !Section.getObject()) {
        if (demangle) {
            char *oldname = (*frames)[0].func_name;
            if (oldname != NULL) {
                std::pair<char *, bool> demangled = jl_demangle(oldname);
                (*frames)[0].func_name = demangled.first;
                (*frames)[0].fromC = !demangled.second;
                free(oldname);
            }
            else {
                // We do this to hide the jlcall wrappers when getting julia backtraces,
                // but it is still good to have them for regular lookup of C frames.
                // Technically not true, but we don't want them
                // in julia backtraces, so close enough
                (*frames)[0].fromC = 1;
            }
        }
        return 1;
    }
    DILineInfoSpecifier infoSpec(DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                                 DILineInfoSpecifier::FunctionNameKind::ShortName);

    // DWARFContext/DWARFUnit update some internal tables during these queries, so
    // a lock is needed.
    if (!jl_lock_profile_wr())
        return lookup_pointer(object::SectionRef(), NULL, frames, pointer, slide, demangle, noInline);
    auto inlineInfo = context->getInliningInfoForAddress(makeAddress(Section, pointer + slide), infoSpec);
    jl_unlock_profile_wr();

    int fromC = (*frames)[0].fromC;
    int n_frames = inlineInfo.getNumberOfFrames();
    if (n_frames == 0) {
        // no line number info available in the context, return without the context
        return lookup_pointer(object::SectionRef(), NULL, frames, pointer, slide, demangle, noInline);
    }
    if (noInline)
        n_frames = 1;
    if (n_frames > 1) {
        jl_frame_t *new_frames = (jl_frame_t*)calloc(sizeof(jl_frame_t), n_frames);
        memcpy(&new_frames[n_frames - 1], *frames, sizeof(jl_frame_t));
        free(*frames);
        *frames = new_frames;
    }
    for (int i = 0; i < n_frames; i++) {
        bool inlined_frame = i != n_frames - 1;
        DILineInfo info;
        if (!noInline) {
            info = inlineInfo.getFrame(i);
        }
        else {
            int havelock = jl_lock_profile_wr();
            assert(havelock); (void)havelock;
            info = context->getLineInfoForAddress(makeAddress(Section, pointer + slide), infoSpec);
            jl_unlock_profile_wr();
        }

        jl_frame_t *frame = &(*frames)[i];
        std::string func_name(info.FunctionName);

        if (inlined_frame) {
            frame->inlined = 1;
            frame->fromC = fromC;
            if (!fromC) {
                std::size_t semi_pos = func_name.find(';');
                if (semi_pos != std::string::npos) {
                    func_name = func_name.substr(0, semi_pos);
                    frame->ci = NULL; // Looked up on Julia side
                }
            }
        }

        if (func_name == "<invalid>")
            frame->func_name = NULL;
        else
            jl_copy_str(&frame->func_name, func_name.c_str());
        if (!frame->func_name)
            frame->fromC = 1;

        frame->line = info.Line;
        std::string file_name(info.FileName);

        if (file_name == "<invalid>")
            frame->file_name = NULL;
        else
            jl_copy_str(&frame->file_name, file_name.c_str());
    }
    return n_frames;
}

#ifdef _OS_DARWIN_
#include <mach-o/dyld.h>
#else
#define LC_UUID 0
#endif
#ifndef _OS_WINDOWS_
#include <dlfcn.h>
#endif



#if defined(_OS_DARWIN_) && defined(LLVM_SHLIB)

void JITDebugInfoRegistry::libc_frames_t::libc_register_frame(const char *Entry) {
    frame_register_func libc_register_frame_ = jl_atomic_load_relaxed(&this->libc_register_frame_);
    if (!libc_register_frame_) {
        libc_register_frame_ = (void(*)(void*))dlsym(RTLD_NEXT, "__register_frame");
        jl_atomic_store_release(&this->libc_register_frame_, libc_register_frame_);
    }
    assert(libc_register_frame_);
    jl_profile_atomic([&]() JL_NOTSAFEPOINT {
        libc_register_frame_(const_cast<char *>(Entry));
        __register_frame(const_cast<char *>(Entry));
    });
}

void JITDebugInfoRegistry::libc_frames_t::libc_deregister_frame(const char *Entry) {
    frame_register_func libc_deregister_frame_  = jl_atomic_load_relaxed(&this->libc_deregister_frame_);
    if (!libc_deregister_frame_) {
        libc_deregister_frame_ = (void(*)(void*))dlsym(RTLD_NEXT, "__deregister_frame");
        jl_atomic_store_release(&this->libc_deregister_frame_, libc_deregister_frame_);
    }
    assert(libc_deregister_frame_);
    jl_profile_atomic([&]() JL_NOTSAFEPOINT {
        libc_deregister_frame_(const_cast<char *>(Entry));
        __deregister_frame(const_cast<char *>(Entry));
    });
}
#endif

static bool getObjUUID(const object::MachOObjectFile *obj, uint8_t uuid[16]) JL_NOTSAFEPOINT
{
    for (auto Load : obj->load_commands())
    {
        if (Load.C.cmd == LC_UUID) {
            memcpy(uuid, ((const MachO::uuid_command*)Load.Ptr)->uuid, 16);
            return true;
        }
    }
    return false;
}
static debug_link_info getDebuglink(const object::ObjectFile &Obj) JL_NOTSAFEPOINT
{
    debug_link_info info = {};
    for (const object::SectionRef &Section: Obj.sections()) {
        Expected<StringRef> sName = Section.getName();
        if (sName && *sName == ".gnu_debuglink")
        {
            auto found = Section.getContents();
            if (found) {
                StringRef Contents = *found;
                size_t length = Contents.find('\0');
                info.filename = Contents.substr(0, length);
                info.crc32 = *(const uint32_t*)Contents.substr(LLT_ALIGN(length + 1, 4), 4).data();
                break;
            }
        }
    }
    return info;
}
/*
 * crc function from http://svnweb.freebsd.org/base/head/sys/libkern/crc32.c (and lldb)
 *
 *   COPYRIGHT (C) 1986 Gary S. Brown. You may use this program, or
 *   code or tables extracted from it, as desired without restriction.
 */
static uint32_t
calc_gnu_debuglink_crc32(const void *buf, size_t size) JL_NOTSAFEPOINT
{
    static const uint32_t g_crc32_tab[] =
    {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
        0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
        0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
        0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
        0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
        0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
        0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
        0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
        0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
        0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
        0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
        0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
        0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
        0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
        0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
        0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
        0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
        0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
        0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
        0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
        0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
        0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
        0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
        0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
        0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
        0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
        0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
        0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
        0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
        0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
        0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
        0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
        0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
        0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
        0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
        0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
        0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
    };
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc;

    crc = ~0U;
    while (size--)
        crc = g_crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ ~0U;
}

static Expected<object::OwningBinary<object::ObjectFile>>
openDebugInfo(StringRef debuginfopath, const debug_link_info &info) JL_NOTSAFEPOINT
{
    auto SplitFile = MemoryBuffer::getFile(debuginfopath);
    if (std::error_code EC = SplitFile.getError()) {
        return errorCodeToError(EC);
    }

    uint32_t crc32 = calc_gnu_debuglink_crc32(
            SplitFile.get()->getBufferStart(),
            SplitFile.get()->getBufferSize());
    if (crc32 != info.crc32) {
        return errorCodeToError(object::object_error::arch_not_found);
    }

    auto error_splitobj = object::ObjectFile::createObjectFile(
            SplitFile.get().get()->getMemBufferRef(),
            file_magic::unknown);
    if (!error_splitobj) {
        return error_splitobj.takeError();
    }

    // successfully validated and loaded split debug info file
    return object::OwningBinary<object::ObjectFile>(
            std::move(error_splitobj.get()),
            std::move(SplitFile.get()));
}
extern "C" JL_DLLEXPORT_CODEGEN
void jl_register_fptrs_impl(uint64_t image_base, const jl_image_fptrs_t *fptrs,
    jl_code_instance_t **cinfos, size_t n)
{
    getJITDebugRegistry().add_image_info({(uintptr_t) image_base, *fptrs, cinfos, n});
}

template<typename T>
static inline void ignoreError(T &err) JL_NOTSAFEPOINT
{
#if !defined(NDEBUG) // Needed only with LLVM assertion build
    consumeError(err.takeError());
#endif
}

static void get_function_name_and_base(llvm::object::SectionRef Section, std::map<uintptr_t, StringRef, std::greater<size_t>> *symbolmap,
                                       size_t pointer, int64_t slide, bool inimage,
                                       void **saddr, char **name, bool untrusted_dladdr) JL_NOTSAFEPOINT
{
    bool needs_saddr = saddr && (!*saddr || untrusted_dladdr);
    bool needs_name = name && (!*name || untrusted_dladdr);
    // Try platform specific methods first since they are usually faster
    if (needs_saddr) {
#if (defined(_OS_LINUX_) || defined(_OS_FREEBSD_)) && !defined(JL_DISABLE_LIBUNWIND)
        unw_proc_info_t pip;
        // Seems that libunwind may return NULL IP depending on what info it finds...
        if (unw_get_proc_info_by_ip(unw_local_addr_space, pointer,
                                    &pip, NULL) == 0 && pip.start_ip) {
            *saddr = (void*)pip.start_ip;
            needs_saddr = false;
        }
#endif
#if defined(_OS_WINDOWS_) && defined(_CPU_X86_64_)
        DWORD64 ImageBase;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pointer, &ImageBase, NULL);
        if (fn) {
            *saddr = (void*)(ImageBase + fn->BeginAddress);
            needs_saddr = false;
        }
#endif
    }
    if (Section.getObject() && (needs_saddr || needs_name)) {
        uintptr_t addr = 0;
        StringRef nameref{};
        {
            std::shared_lock<std::shared_mutex> read_lock(getJITDebugRegistry().symbol_mutex);
            if (symbolmap->empty()) {
                read_lock.unlock();
                {
                    // symbol map hasn't been generated yet, so fill it in now
                    std::unique_lock<std::shared_mutex> write_lock(getJITDebugRegistry().symbol_mutex);
                    if (symbolmap->empty()) {
                        for (auto sym : Section.getObject()->symbols()) {
                            if (!Section.containsSymbol(sym))
                                continue;

                            auto maybe_addr = sym.getAddress();
                            if (!maybe_addr)
                                continue;
                            size_t addr = maybe_addr.get();

                            auto maybe_nameref = sym.getName();
                            StringRef nameref{};
                            if (maybe_nameref)
                                nameref = maybe_nameref.get();

                            symbolmap->emplace(addr, nameref);
                        }
                    }
                }
                read_lock.lock();
            }
            auto fit = symbolmap->lower_bound(pointer + slide);
            if (fit != symbolmap->end()) {
                addr = fit->first;
                nameref = fit->second;
            }
        }
        std::string namerefstr = nameref.str();
        if (needs_saddr && addr != 0) {
            *saddr = (void*)(addr - slide);
            needs_saddr = false;
        }
        if (needs_name && !nameref.empty()) {
            const char globalPrefix = // == DataLayout::getGlobalPrefix
#if defined(_OS_WINDOWS_) && !defined(_CPU_X86_64_)
                '_';
#elif defined(_OS_DARWIN_)
                '_';
#else
                '\0';
#endif
            if (globalPrefix) {
                if (nameref[0] == globalPrefix)
                  nameref = nameref.drop_front();
#if defined(_OS_WINDOWS_) && !defined(_CPU_X86_64_)
                else if (nameref[0] == '@') // X86_VectorCall
                  nameref = nameref.drop_front();
#endif
                // else VectorCall, Assembly, Internal, etc.
            }
#if defined(_OS_WINDOWS_) && !defined(_CPU_X86_64_)
            nameref = nameref.split('@').first;
#endif
            size_t len = nameref.size();
            *name = (char*)realloc_s(*name, len + 1);
            memcpy(*name, nameref.data(), len);
            (*name)[len] = 0;
            needs_name = false;
        }
    }
#ifdef _OS_WINDOWS_
    // For ntdll and msvcrt since we are currently only parsing DWARF debug info through LLVM
    if (!inimage && needs_name) {
        static char frame_info_func[
            sizeof(SYMBOL_INFO) +
            MAX_SYM_NAME * sizeof(TCHAR)];
        DWORD64 dwDisplacement64 = 0;
        DWORD64 dwAddress = pointer;
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)frame_info_func;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;
        uv_mutex_lock(&jl_in_stackwalk);
        if (SymFromAddr(GetCurrentProcess(), dwAddress, &dwDisplacement64, pSymbol)) {
            // errors are ignored
            jl_copy_str(name, pSymbol->Name);
        }
        uv_mutex_unlock(&jl_in_stackwalk);
    }
#endif
}

static jl_object_file_entry_t find_object_file(uint64_t fbase, StringRef fname) JL_NOTSAFEPOINT
{
    int isdarwin = 0, islinux = 0, iswindows = 0;
#if defined(_OS_DARWIN_)
    isdarwin = 1;
#elif defined(_OS_LINUX_) || defined(_OS_FREEBSD_)
    islinux = 1;
#elif defined(_OS_WINDOWS_)
    iswindows = 1;
#endif
    (void)iswindows;

// GOAL: Read debuginfo from file
    jl_object_file_entry_t entry{nullptr, nullptr, 0, nullptr};
    auto success = getJITDebugRegistry().get_objfile_map()->emplace(fbase, entry);
    if (!success.second)
        // Return cached value
        return success.first->second;

// GOAL: Assign errorobj
    StringRef objpath;
    std::string debuginfopath;
    uint8_t uuid[16], uuid2[16];
    if (isdarwin) {
        size_t msize = (size_t)(((uint64_t)-1) - fbase);
        std::unique_ptr<MemoryBuffer> membuf = MemoryBuffer::getMemBuffer(
                StringRef((const char *)fbase, msize), "", false);
        auto origerrorobj = llvm::object::ObjectFile::createObjectFile(
            membuf->getMemBufferRef(), file_magic::unknown);
        if (!origerrorobj) {
            ignoreError(origerrorobj);
            return entry;
        }

        const object::MachOObjectFile *morigobj = dyn_cast<const object::MachOObjectFile>(
            origerrorobj.get().get());

        // First find the uuid of the object file (we'll use this to make sure we find the
        // correct debug symbol file).
        if (!morigobj || !getObjUUID(morigobj, uuid))
            return entry;

        // Hide Darwin symbols (e.g. CoreFoundation) from non-Darwin systems.
#ifndef _OS_DARWIN_
        return entry;
#else
        // On macOS, debug symbols are not contained in the dynamic library.
        // Use DBGCopyFullDSYMURLForUUID from the private DebugSymbols framework
        // to make use of spotlight to find the dSYM file. If that fails, lookup
        // the dSYM file in the same directory as the dynamic library.  TODO: If
        // the DebugSymbols framework is moved or removed, an alternative would
        // be to directly query Spotlight for the dSYM bundle.

        typedef CFURLRef (*DBGCopyFullDSYMURLForUUIDfn)(CFUUIDRef, CFURLRef) JL_NOTSAFEPOINT;
        DBGCopyFullDSYMURLForUUIDfn DBGCopyFullDSYMURLForUUID = NULL;

        // First, try to load the private DebugSymbols framework.
        CFURLRef dsfmwkurl = CFURLCreateWithFileSystemPath(
            kCFAllocatorDefault,
            CFSTR("/System/Library/PrivateFrameworks/DebugSymbols.framework"),
            kCFURLPOSIXPathStyle, true);
        CFBundleRef dsfmwkbundle =
            CFBundleCreate(kCFAllocatorDefault, dsfmwkurl);
        CFRelease(dsfmwkurl);

        if (dsfmwkbundle) {
            DBGCopyFullDSYMURLForUUID =
                (DBGCopyFullDSYMURLForUUIDfn)CFBundleGetFunctionPointerForName(
                    dsfmwkbundle, CFSTR("DBGCopyFullDSYMURLForUUID"));
        }

        if (DBGCopyFullDSYMURLForUUID != NULL) {
            // Prepare UUID and shared object path URL.
            CFUUIDRef objuuid = CFUUIDCreateWithBytes(
                kCFAllocatorDefault, uuid[0], uuid[1], uuid[2], uuid[3],
                uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10],
                uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
            CFURLRef objurl = CFURLCreateFromFileSystemRepresentation(
                kCFAllocatorDefault, (UInt8 const *)fname.data(),
                (CFIndex)strlen(fname.data()), FALSE);

            // Call private DBGCopyFullDSYMURLForUUID() to find dSYM.
            CFURLRef dsympathurl = DBGCopyFullDSYMURLForUUID(objuuid, objurl);
            CFRelease(objuuid);
            CFRelease(objurl);

            char objpathcstr[JL_PATH_MAX];
            if (dsympathurl != NULL &&
                CFURLGetFileSystemRepresentation(
                    dsympathurl, true, (UInt8 *)objpathcstr,
                    (CFIndex)sizeof(objpathcstr))) {
                // The dSYM was found. Copy its path.
                debuginfopath = objpathcstr;
                objpath = debuginfopath;
                CFRelease(dsympathurl);
            }
        }

        if (dsfmwkbundle) {
            CFRelease(dsfmwkbundle);
        }
#endif

        if (objpath.empty()) {
            // Fall back to simple path relative to the dynamic library.
            size_t sep = fname.rfind('/');
            debuginfopath = fname.str();
            debuginfopath += ".dSYM/Contents/Resources/DWARF/";
            debuginfopath += fname.substr(sep + 1);
            objpath = debuginfopath;
        }
    }
    else {
        // On Linux systems we need to mmap another copy because of the permissions on the mmap'ed shared library.
        // On Windows we need to mmap another copy since reading the in-memory copy seems to return object_error:unexpected_eof
        objpath = fname;
    }
    auto errorobj = llvm::object::ObjectFile::createObjectFile(objpath);

// GOAL: Find obj, context, slide (if above succeeded)
    if (errorobj) {
        auto *debugobj = errorobj->getBinary();

        if (islinux) {
            // if the file has a .gnu_debuglink section,
            // try to load its companion file instead
            // in the expected locations
            // for now, we don't support the build-id method
            debug_link_info info = getDebuglink(*debugobj);
            if (!info.filename.empty()) {
                size_t sep = fname.rfind('/');
                Expected<object::OwningBinary<object::ObjectFile>>
                    DebugInfo(errorCodeToError(std::make_error_code(std::errc::no_such_file_or_directory)));
                // Can't find a way to construct an empty Expected object
                // that can be ignored.
                if (fname.substr(sep + 1) != info.filename) {
                    debuginfopath = fname.substr(0, sep + 1).str();
                    debuginfopath += info.filename;
                    ignoreError(DebugInfo);
                    DebugInfo = openDebugInfo(debuginfopath, info);
                }
                if (!DebugInfo) {
                    debuginfopath = fname.substr(0, sep + 1).str();
                    debuginfopath += ".debug/";
                    debuginfopath += info.filename;
                    ignoreError(DebugInfo);
                    DebugInfo = openDebugInfo(debuginfopath, info);
                }
                if (!DebugInfo) {
                    debuginfopath = "/usr/lib/debug/";
                    debuginfopath += fname.substr(0, sep + 1);
                    debuginfopath += info.filename;
                    ignoreError(DebugInfo);
                    DebugInfo = openDebugInfo(debuginfopath, info);
                }
                if (DebugInfo) {
                    errorobj = std::move(DebugInfo);
                    // Yes, we've checked, and yes LLVM want us to check again.
                    ignoreError(errorobj);
                    debugobj = errorobj->getBinary();
                }
                else {
                    ignoreError(DebugInfo);
                }
            }
        }

        if (isdarwin) {
            // verify the UUID matches
            if (!isa<const object::MachOObjectFile>(debugobj) ||
                !getObjUUID(cast<const object::MachOObjectFile>(debugobj), uuid2) ||
                memcmp(uuid, uuid2, sizeof(uuid)) != 0) {
                return entry;
            }
        }

        int64_t slide = 0;
        if (auto *OF = dyn_cast<const object::COFFObjectFile>(debugobj)) {
            if (!iswindows) // the COFF parser accepts some garbage inputs (like empty files) that the other parsers correctly reject, so we can end up here even when we should not
                return entry;
            slide = OF->getImageBase() - fbase;
        }
        else {
            slide = -(int64_t)fbase;
        }

        auto context = DWARFContext::create(*debugobj).release();
        auto binary = errorobj->takeBinary();
        binary.first.release();
        binary.second.release();

        entry = {debugobj, context, slide, new std::map<uintptr_t, StringRef, std::greater<size_t>>()};
        // update cache
        (*getJITDebugRegistry().get_objfile_map())[fbase] = entry;
    }
    else {
        // TODO: report the error instead of silently consuming it?
        //       jl_error might run into the same error again...
        ignoreError(errorobj);
    }
    return entry;
}

// from llvm::SymbolizableObjectFile
static object::SectionRef getModuleSectionForAddress(const object::ObjectFile *obj, uint64_t Address) JL_NOTSAFEPOINT
{
  for (object::SectionRef Sec : obj->sections()) {
      if (!Sec.isText() || Sec.isVirtual())
          continue;
      if (Address >= Sec.getAddress() && Address < Sec.getAddress() + Sec.getSize())
          return Sec;
  }
  return object::SectionRef();
}


bool jl_dylib_DI_for_fptr(size_t pointer, object::SectionRef *Section, int64_t *slide, llvm::DIContext **context,
    bool onlyImage, bool *isImage, uint64_t *_fbase, void **saddr, char **name, char **filename) JL_NOTSAFEPOINT
{
    *Section = object::SectionRef();
    *context = NULL;
    // On Windows and FreeBSD, `dladdr` (or its equivalent) returns the closest exported symbol
    // without checking the size.
    // This causes the lookup to return incorrect non-NULL result for local functions
    // when better result is available through other methods.
    // macOS's `dladdr` returns local symbols and Linux's `dladdr`
    // checks the symbol size so they do not have this problem.
    // On systems with an untrusted dladdr, the result cannot be used for sysimg
    // (it's always wrong) and should in general be used only as the last fallback.
#if defined(_OS_FREEBSD_) || defined(_OS_WINDOWS_)
    bool untrusted_dladdr = true;
#else
    bool untrusted_dladdr = false;
#endif

// GOAL: Determine containing Library
// Assigning fname, fbase
#ifdef _OS_WINDOWS_
    IMAGEHLP_MODULE64 ModuleInfo;
    ModuleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
    uv_mutex_lock(&jl_in_stackwalk);
    jl_refresh_dbg_module_list();
    bool isvalid = SymGetModuleInfo64(GetCurrentProcess(), (DWORD64)pointer, &ModuleInfo);
    uv_mutex_unlock(&jl_in_stackwalk);
    if (!isvalid)
        return false;

    StringRef fname = ModuleInfo.LoadedImageName;
    if (fname.empty()) // empirically, LoadedImageName might be missing
        fname = ModuleInfo.ImageName;
    DWORD64 fbase = ModuleInfo.BaseOfImage;
    JITDebugInfoRegistry::image_info_t image_info;
    bool inimage = getJITDebugRegistry().get_image_info(fbase, &image_info);
    if (isImage)
        *isImage = inimage;
    if (onlyImage && !inimage)
        return false;
    // If we didn't find the filename before in the debug
    // info, use the dll name
    if (filename && !*filename)
        jl_copy_str(filename, fname.data());
    if (saddr)
        *saddr = NULL;
    if (_fbase)
        *_fbase = fbase;

#else // ifdef _OS_WINDOWS_
    Dl_info dlinfo;
    int dladdr_success;
    uint64_t fbase;
#ifdef __GLIBC__
    struct link_map *extra_info;
    dladdr_success = dladdr1((void*)pointer, &dlinfo, (void**)&extra_info, RTLD_DL_LINKMAP) != 0;
    if (dladdr_success) {
        msan_unpoison(&dlinfo, sizeof(dlinfo));
        if (dlinfo.dli_fname)
            msan_unpoison_string(dlinfo.dli_fname);
        if (dlinfo.dli_sname)
            msan_unpoison_string(dlinfo.dli_sname);
        msan_unpoison(&extra_info, sizeof(struct link_map*));
        msan_unpoison(extra_info, sizeof(struct link_map));
    }
#else
#ifdef _OS_DARWIN_
    // On macOS 12, dladdr(-1, …) succeeds and returns the main executable image,
    // despite there never actually being an image there. This is not what we want,
    // as we use -1 as a known-invalid value e.g. in the test suite.
    if (pointer == ~(size_t)0) {
        return false;
    }
#endif
    dladdr_success = dladdr((void*)pointer, &dlinfo) != 0;
#endif
    if (!dladdr_success || !dlinfo.dli_fname)
        return false;

#ifdef __GLIBC__
    // dlinfo.dli_fbase is not the right value for the main executable on linux
    fbase = (uintptr_t)extra_info->l_addr;
#else
    fbase = (uintptr_t)dlinfo.dli_fbase;
#endif
    StringRef fname;
    JITDebugInfoRegistry::image_info_t image_info;
    bool inimage = getJITDebugRegistry().get_image_info(fbase, &image_info);
    if (saddr && !(inimage && untrusted_dladdr))
        *saddr = dlinfo.dli_saddr;
    if (isImage)
        *isImage = inimage;
    if (onlyImage && !inimage)
        return false;
    if (_fbase)
        *_fbase = fbase;
    // In case we fail with the debug info lookup, we at least still
    // have the function name, even if we don't have line numbers
    if (name && !(inimage && untrusted_dladdr))
        jl_copy_str(name, dlinfo.dli_sname);
    if (filename)
        jl_copy_str(filename, dlinfo.dli_fname);
    fname = dlinfo.dli_fname;
#endif // ifdef _OS_WINDOWS_
    jl_object_file_entry_t entry = find_object_file(fbase, fname);
    *slide = entry.slide;
    *context = entry.ctx;
    if (entry.obj)
        *Section = getModuleSectionForAddress(entry.obj, pointer + entry.slide);
    // Assume we only need base address for sysimg for now
    if (!inimage || 0 == image_info.fptrs.nptrs)
        saddr = nullptr;
    get_function_name_and_base(*Section, entry.symbolmap, pointer, entry.slide, inimage, saddr, name, untrusted_dladdr);
    return true;
}

// *name and *filename should be either NULL or malloc'd pointer
static int jl_getDylibFunctionInfo(jl_frame_t **frames, size_t pointer, int skipC, int noInline) JL_NOTSAFEPOINT
{
    // This function is not allowed to reference any TLS variables if noInline
    // since it can be called from an unmanaged thread (the segfault handler)
    jl_frame_t *frame0 = *frames;
#ifdef _OS_WINDOWS_
    static IMAGEHLP_LINE64 frame_info_line;
    DWORD dwDisplacement = 0;
    uv_mutex_lock(&jl_in_stackwalk);
    jl_refresh_dbg_module_list();
    DWORD64 dwAddress = pointer;
    frame_info_line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    if (SymGetLineFromAddr64(GetCurrentProcess(), dwAddress, &dwDisplacement, &frame_info_line)) {
        // SymGetLineFromAddr64 returned success
        // record source file name and line number
        if (frame_info_line.FileName)
            jl_copy_str(&frame0->file_name, frame_info_line.FileName);
        frame0->line = frame_info_line.LineNumber;
    }
    uv_mutex_unlock(&jl_in_stackwalk);
#endif
    object::SectionRef Section;
    llvm::DIContext *context = NULL;
    int64_t slide;
    bool isImage;
    void *saddr;
    uint64_t fbase;
    if (!jl_dylib_DI_for_fptr(pointer, &Section, &slide, &context, skipC, &isImage, &fbase, &saddr, &frame0->func_name, &frame0->file_name)) {
        frame0->fromC = 1;
        return 1;
    }
    frame0->fromC = !isImage;
    {
        JITDebugInfoRegistry::image_info_t image;
        bool inimage = getJITDebugRegistry().get_image_info(fbase, &image);
        if (isImage && saddr && inimage) {
            for (size_t i = 0; i < image.fptrs.nclones; i++) {
                if (saddr == image.fptrs.clone_ptrs[i]) {
                    uint32_t idx = image.fptrs.clone_idxs[i] & jl_sysimg_val_mask;
                    if (idx < image.fvars_n) // items after this were cloned but not referenced directly by a method (such as our ccall PLT thunks)
                        frame0->ci = image.fvars_cinst[idx];
                    break;
                }
            }
            for (size_t i = 0; i < image.fvars_n; i++) {
                if (saddr == image.fptrs.ptrs[i]) {
                    frame0->ci = image.fvars_cinst[i];
                    break;
                }
            }
        }
    }
    return lookup_pointer(Section, context, frames, pointer, slide, isImage, noInline);
}

int jl_DI_for_fptr(uint64_t fptr, uint64_t *symsize, int64_t *slide,
        object::SectionRef *Section, llvm::DIContext **context) JL_NOTSAFEPOINT
{
    int found = 0;
    if (!jl_lock_profile_wr())
        return 0;

    if (symsize)
        *symsize = 0;

    auto &objmap = getJITDebugRegistry().getObjectMap();
    auto fit = objmap.lower_bound(fptr);
    if (fit != objmap.end() && fptr < fit->first + fit->second.SectionSize) {
        *slide = fit->second.slide;
        auto lazyobject = fit->second.object;
        if (!lazyobject->object && !lazyobject->data.empty()) {
            if (lazyobject->uncompressedsize) {
                SmallVector<uint8_t, 0> unpacked;
                compression::Format F = compression::zstd::isAvailable() ? compression::Format::Zstd : compression::Format::Zlib;
                Error E = compression::decompress(F, lazyobject->data, unpacked, lazyobject->uncompressedsize);
                if (E)
                    lazyobject->data.clear();
                else
                    lazyobject->data = std::move(unpacked);
                jl_jit_add_bytes(lazyobject->data.size() - lazyobject->uncompressedsize);
                lazyobject->uncompressedsize = 0;
            }
            if (!lazyobject->data.empty()) {
                auto obj = object::ObjectFile::createObjectFile(MemoryBufferRef(StringRef((const char*)lazyobject->data.data(), lazyobject->data.size()), "jit.o"));
                if (obj)
                    lazyobject->object = std::move(*obj);
                else
                    lazyobject->data.clear();
            }
        }
        if (lazyobject->object) {
            *Section = *std::next(lazyobject->object->section_begin(), fit->second.SectionIndex);
            if (context) {
                if (lazyobject->context == nullptr)
                    lazyobject->context = DWARFContext::create(*lazyobject->object);
                *context = lazyobject->context.get();
            }
        }
        found = 1;
    }
    jl_unlock_profile_wr();
    return found;
}

// Set *name and *filename to either NULL or malloc'd string
extern "C" JL_DLLEXPORT_CODEGEN int jl_getFunctionInfo_impl(jl_frame_t **frames_out, size_t pointer, int skipC, int noInline) JL_NOTSAFEPOINT
{
    // This function is not allowed to reference any TLS variables if noInline
    // since it can be called from an unmanaged thread on OSX.

    jl_frame_t *frames = (jl_frame_t*)calloc(sizeof(jl_frame_t), 1);
    frames[0].line = -1;
    *frames_out = frames;

    llvm::DIContext *context = nullptr;
    object::SectionRef Section;
    int64_t slide;
    uint64_t symsize;
    if (jl_DI_for_fptr(pointer, &symsize, &slide, &Section, &context)) {
        frames[0].ci = getJITDebugRegistry().lookupCodeInstance(pointer);
        int nf = lookup_pointer(Section, context, frames_out, pointer, slide, true, noInline);
        return nf;
    }
    return jl_getDylibFunctionInfo(frames_out, pointer, skipC, noInline);
}

extern "C" jl_code_instance_t *jl_gdblookupci(void *p) JL_NOTSAFEPOINT
{
    return getJITDebugRegistry().lookupCodeInstance((size_t)p);
}

#if defined(_OS_DARWIN_) && defined(LLVM_SHLIB)

/*
 * We use a custom unwinder, so we need to make sure that when registering dynamic
 * frames, we do so with our unwinder rather than with the system one. If LLVM is
 * statically linked everything works out fine, but if it's dynamically linked
 * it would usually pick up the system one, so we need to do the registration
 * ourselves to ensure the right one gets picked.
 */

// This implementation handles frame registration for local targets.
void register_eh_frames(uint8_t *Addr, size_t Size)
{
  // On OS X OS X __register_frame takes a single FDE as an argument.
  // See http://lists.cs.uiuc.edu/pipermail/llvmdev/2013-April/061768.html
  processFDEs((char*)Addr, Size, [](const char *Entry) JL_NOTSAFEPOINT {
      getJITDebugRegistry().libc_frames.libc_register_frame(Entry);
    });
}

void deregister_eh_frames(uint8_t *Addr, size_t Size)
{
   processFDEs((char*)Addr, Size, [](const char *Entry) JL_NOTSAFEPOINT {
      getJITDebugRegistry().libc_frames.libc_deregister_frame(Entry);
    });
}

#elif (defined(_OS_LINUX_) || defined(_OS_FREEBSD_)) && \
    defined(JL_UNW_HAS_FORMAT_IP) && \
    !defined(_CPU_ARM_) // ARM does not have/use .eh_frame, so we handle this elsewhere
#include <type_traits>

// Skip over an arbitrary long LEB128 encoding.
// Return the pointer to the first unprocessed byte.
static const uint8_t *consume_leb128(const uint8_t *Addr, const uint8_t *End) JL_NOTSAFEPOINT
{
    const uint8_t *P = Addr;
    while ((*P >> 7) != 0 && P < End)
        ++P;
    return P + 1;
}

// Parse a LEB128 encoding to a type T. Truncate the result if there's more
// bytes than what there are more bytes than what the type can store.
// Adjust the pointer to the first unprocessed byte.
template<typename T> static T parse_leb128(const uint8_t *&Addr,
                                           const uint8_t *End) JL_NOTSAFEPOINT
{
    typedef typename std::make_unsigned<T>::type uT;
    uT v = 0;
    for (unsigned i = 0;i < ((sizeof(T) * 8 - 1) / 7 + 1);i++) {
        uint8_t a = *Addr;
        Addr++;
        v |= uT(a & 0x7f) << (i * 7);
        if ((a & 0x80) == 0 || Addr >= End) {
            if (a & 0x40 && std::is_signed<T>::value) {
                int valid_bits = (i + 1) * 7;
                if (valid_bits < 64) {
                    v |= -(uT(1) << valid_bits);
                }
            }
            return T(v);
        }
    }
    Addr = consume_leb128(Addr, End);
    return T(v);
}

template <typename U, typename T>
static U safe_trunc(T t) JL_NOTSAFEPOINT
{
    assert((t >= static_cast<T>(std::numeric_limits<U>::min()))
           && (t <= static_cast<T>(std::numeric_limits<U>::max())));
    return static_cast<U>(t);
}

// How the address and size in the FDE are encoded.
enum DW_EH_PE : uint8_t {
    DW_EH_PE_absptr = 0x00, /* An absolute pointer. The size is determined by
                             * whether this is a 32-bit or 64-bit address space,
                             * and will be 32 or 64 bits */
    DW_EH_PE_omit = 0xff, // The value is omitted
    DW_EH_PE_uleb128 = 0x01, // The value is an unsigned LEB128
    DW_EH_PE_udata2 = 0x02,
    DW_EH_PE_udata4 = 0x03,
    DW_EH_PE_udata8 = 0x04, /* The value is stored as unsigned data with the
                             * specified number of bytes. */
    DW_EH_PE_signed = 0x08, /* A signed number. The size is determined by
                             * whether this is a 32-bit or 64-bit address space */
    DW_EH_PE_sleb128 = 0x09, /* A signed LEB128. */
    DW_EH_PE_sdata2 = 0x0a,
    DW_EH_PE_sdata4 = 0x0b,
    DW_EH_PE_sdata8 = 0x0c, /* The value is stored as signed data with the
                             * specified number of bytes. */

    // In addition the above basic encodings, there are modifiers.

    DW_EH_PE_pcrel = 0x10, // Value is PC relative.

    // We currently don't support the following once.
    DW_EH_PE_textrel = 0x20, // Value is text relative.
    DW_EH_PE_datarel = 0x30, // Value is data relative.
    DW_EH_PE_funcrel = 0x40, // Value is relative to start of function.
    DW_EH_PE_aligned = 0x50, /* Value is aligned: padding bytes are inserted as
                              * required to make value be naturally aligned. */
    DW_EH_PE_indirect = 0x80 /* This is actually the address of the real value. */
};

// Parse the CIE and return the type of encoding used by FDE
static DW_EH_PE parseCIE(const uint8_t *Addr, const uint8_t *End) JL_NOTSAFEPOINT
{
    // https://www.airs.com/blog/archives/460
    // Length (4 bytes)
    uint32_t cie_size = *(const uint32_t*)Addr;
    const uint8_t *cie_addr = Addr + 4;
    const uint8_t *p = cie_addr;
    const uint8_t *cie_end = cie_addr + cie_size;
    assert(cie_end <= End);
    // Check this is an CIE record (CIE ID: 4 bytes)
    assert(*(const uint32_t*)cie_addr == 0);
    p += 4;
    // Check CIE version (1 byte)
    uint8_t cie_version = *p;
    assert(cie_version == 1 || cie_version == 3);
    p++;
    // Augmentation String (NUL terminate)
    const char *augmentation = (const char*)p;
    size_t augmentation_len = strlen(augmentation);
    // Assume there's no EH Data field, which exist when the augmentation
    // string has "eh" in it.
    p += augmentation_len + 1;
    // Code Alignment Factor (1 byte)
    // should always be 1 on x86, 4 on PPC, etc.
    // (used for DW_CFA_advance_loc / not used here)
    //assert(*p == 1);
    p++;
    // Data Alignment Factor (LEB128)
    assert(cie_end >= p);
    p = consume_leb128(p, cie_end);
    // return address register
    if (cie_version == 1) {
        p++;
    }
    else {
        p = consume_leb128(p, cie_end);
    }
    // Now it's the augmentation data. which may have the information we
    // are interested in...
    for (const char *augp = augmentation;;augp++) {
        switch (*augp) {
        case 'z':
            // Augmentation Length
            p = consume_leb128(p, cie_end);
            break;
        case 'L':
            // LSDA encoding
            p++;
            break;
        case 'R':
            // .... the only one we care about ....
            return static_cast<DW_EH_PE>(*p);
        case 'P': {
            // Personality data
            // Encoding
            auto encoding = static_cast<DW_EH_PE>(*p);
            p++;
            // Personality function
            switch (encoding & 0xf) {
            case DW_EH_PE_uleb128:
            case DW_EH_PE_sleb128:
                p = consume_leb128(p, cie_end);
                break;
            case DW_EH_PE_udata2:
            case DW_EH_PE_sdata2:
                p += 2;
                break;
            case DW_EH_PE_udata4:
            case DW_EH_PE_sdata4:
                p += 4;
                break;
            case DW_EH_PE_udata8:
            case DW_EH_PE_sdata8:
                p += 8;
                break;
            case DW_EH_PE_signed:
                p += sizeof(void*);
                break;
            default:
                if (encoding == DW_EH_PE_absptr || encoding == DW_EH_PE_omit) {
                    p += sizeof(void*);
                }
                else {
                    assert(0 && "Invalid personality encoding.");
                }
                break;
            }
        }
            break;
        default:
            continue;
        }
        assert(cie_end >= p);
    }
    return DW_EH_PE_absptr;
}

void register_eh_frames(uint8_t *Addr, size_t Size)
{
    // System unwinder
    jl_profile_atomic([&]() JL_NOTSAFEPOINT {
        __register_frame(Addr);
    });

    // Now first count the number of FDEs
    size_t nentries = 0;
    processFDEs((char*)Addr, Size, [&](const char*) JL_NOTSAFEPOINT { nentries++; });
    if (nentries == 0)
        return;

    // Our unwinder
    unw_dyn_info_t *di = new unw_dyn_info_t;
    // In a shared library, this is set to the address of the PLT.
    // For us, just put 0 to emulate a static library. This field does
    // not seem to be used on our supported architectures.
    di->gp = 0;
    // I'm not a great fan of the naming of this constant, but it means the
    // right thing, which is a table of FDEs and IPs.
    di->format = UNW_INFO_FORMAT_IP_OFFSET;
    di->u.rti.name_ptr = 0;
    di->u.rti.segbase = (unw_word_t)Addr;

    uintptr_t start_ip = (uintptr_t)-1;
    uintptr_t end_ip = 0;

    // Then allocate a table and fill in the information
    // While we're at it, also record the start_ip and size,
    // which we fill in the table
    unw_table_entry *table = new unw_table_entry[nentries];
    SmallVector<uintptr_t, 0> start_ips(nentries);
    size_t cur_entry = 0;
    // Cache the previously parsed CIE entry so that we can support multiple
    // CIE's (may not happen) without parsing it every time.
    const uint8_t *cur_cie = nullptr;
    DW_EH_PE encoding = DW_EH_PE_omit;
    processFDEs((char*)Addr, Size, [&](const char *Entry) JL_NOTSAFEPOINT {
            // Skip Length (4bytes) and CIE offset (4bytes)
            uint32_t fde_size = *(const uint32_t*)Entry;
            uint32_t cie_id = ((const uint32_t*)Entry)[1];
            const uint8_t *cie_addr = (const uint8_t*)(Entry + 4 - cie_id);
            if (cie_addr != cur_cie)
                encoding = parseCIE(cie_addr, Addr + Size);
            const uint8_t *fde_end = (const uint8_t*)(Entry + 4 + fde_size);
            const uint8_t *EntryPtr = (const uint8_t*)(Entry + 8);
            uintptr_t start = 0;
            uintptr_t size = 0;
            // The next two fields are address and size of the PC range
            // covered by this FDE.
            if (encoding == DW_EH_PE_absptr || encoding == DW_EH_PE_omit) {
                assert(fde_size >= 2 * sizeof(void*) + 4);
                start = *(const uintptr_t*)EntryPtr;
                size = *(const uintptr_t*)(EntryPtr + sizeof(void*));
            }
            else {
                uintptr_t baseptr = (uintptr_t)EntryPtr;
                // Only support pcrel for now...
                assert((encoding & 0xf0) == 0x10 &&
                       "Only pcrel mode is supported");
                switch (encoding & 0xf) {
                case DW_EH_PE_uleb128:
                    start = baseptr + parse_leb128<uintptr_t>(EntryPtr, fde_end);
                    size = parse_leb128<uintptr_t>(EntryPtr, fde_end);
                    break;
                case DW_EH_PE_udata2:
                    assert(fde_size >= 2 * 2 + 4);
                    start = baseptr + ((const uint16_t*)EntryPtr)[0];
                    size = ((const uint16_t*)EntryPtr)[1];
                    break;
                case DW_EH_PE_udata4:
                    assert(fde_size >= 2 * 4 + 4);
                    start = baseptr + ((const uint32_t*)EntryPtr)[0];
                    size = ((const uint32_t*)EntryPtr)[1];
                    break;
                case DW_EH_PE_udata8:
                    assert(fde_size >= 2 * 8 + 4);
                    start = uintptr_t(baseptr + ((const uint64_t*)EntryPtr)[0]);
                    size = uintptr_t(((const uint64_t*)EntryPtr)[1]);
                    break;
                case DW_EH_PE_signed:
                    assert(fde_size >= 2 * sizeof(void*) + 4);
                    start = baseptr + ((const intptr_t*)EntryPtr)[0];
                    size = ((const intptr_t*)EntryPtr)[1];
                    break;
                case DW_EH_PE_sleb128:
                    start = baseptr + parse_leb128<intptr_t>(EntryPtr, fde_end);
                    size = parse_leb128<intptr_t>(EntryPtr, fde_end);
                    break;
                case DW_EH_PE_sdata2:
                    assert(fde_size >= 2 * 2 + 4);
                    start = baseptr + ((const int16_t*)EntryPtr)[0];
                    size = ((const int16_t*)EntryPtr)[1];
                    break;
                case DW_EH_PE_sdata4:
                    assert(fde_size >= 2 * 4 + 4);
                    start = baseptr + ((const int32_t*)EntryPtr)[0];
                    size = ((const int32_t*)EntryPtr)[1];
                    break;
                case DW_EH_PE_sdata8:
                    assert(fde_size >= 2 * 8 + 4);
                    start = uintptr_t(baseptr + ((const int64_t*)EntryPtr)[0]);
                    size = uintptr_t(((const int64_t*)EntryPtr)[1]);
                    break;
                default:
                    assert(0 && "Invalid FDE encoding.");
                    break;
                }
            }

            if (start < start_ip)
                start_ip = start;
            if (end_ip < (start + size))
                end_ip = start + size;
            table[cur_entry].fde_offset =
                safe_trunc<int32_t>((intptr_t)Entry - (intptr_t)Addr);
            start_ips[cur_entry] = start;
            cur_entry++;
        });
    for (size_t i = 0; i < nentries; i++) {
        table[i].start_ip_offset =
            safe_trunc<int32_t>((intptr_t)start_ips[i] - (intptr_t)start_ip);
    }
    assert(end_ip != 0);

    di->u.rti.table_len = nentries * sizeof(*table) / sizeof(unw_word_t);
    di->u.rti.table_data = (unw_word_t)table;
    di->start_ip = start_ip;
    di->end_ip = end_ip;

    jl_profile_atomic([&]() JL_NOTSAFEPOINT {
        _U_dyn_register(di);
    });
}

void deregister_eh_frames(uint8_t *Addr, size_t Size)
{
    jl_profile_atomic([&]() JL_NOTSAFEPOINT {
        __deregister_frame(Addr);
    });
    // Deregistering with our unwinder (_U_dyn_cancel) requires a lookup table
    // to find the allocated entry above (or looking into libunwind's internal
    // data structures).
}

#else

void register_eh_frames(uint8_t *Addr, size_t Size)
{
}

void deregister_eh_frames(uint8_t *Addr, size_t Size)
{
}

#endif

extern "C" JL_DLLEXPORT_CODEGEN
uint64_t jl_getUnwindInfo_impl(uint64_t dwAddr) JL_NOTSAFEPOINT
{
    // Might be called from unmanaged thread
    jl_lock_profile();
    auto &objmap = getJITDebugRegistry().getObjectMap();
    auto it = objmap.lower_bound(dwAddr);
    uint64_t ipstart = 0; // ip of the start of the section (if found)
    if (it != objmap.end() && dwAddr < it->first + it->second.SectionSize) {
        ipstart = (uint64_t)(uintptr_t)(*it).first;
    }
    jl_unlock_profile();
    return ipstart;
}
