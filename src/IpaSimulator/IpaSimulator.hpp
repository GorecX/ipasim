#include <LIEF/LIEF.hpp>
#include <Windows.h>
#include <cstdarg>
#include <string>
#include <unicorn/unicorn.h>

class DynamicLoader;

class LoadedLibrary {
public:
  LoadedLibrary() : StartAddress(0), Size(0), IsWrapperDLL(false) {}
  virtual ~LoadedLibrary() = default;

  uint64_t StartAddress, Size;
  bool IsWrapperDLL;

  // TODO: Check that the found symbol is inside range [StartAddress, +Size].
  virtual uint64_t findSymbol(DynamicLoader &DL, const std::string &Name) = 0;
  virtual bool hasUnderscorePrefix() = 0;
  bool isInRange(uint64_t Addr);
  void checkInRange(uint64_t Addr);
};

class LoadedDylib : public LoadedLibrary {
public:
  LIEF::MachO::Binary &Bin;

  LoadedDylib(std::unique_ptr<LIEF::MachO::FatBinary> &&Fat)
      : Fat(move(Fat)), Bin(Fat->at(0)) {}
  uint64_t findSymbol(DynamicLoader &DL, const std::string &Name) override;
  bool hasUnderscorePrefix() override { return true; }
  uint64_t getSection(const std::string &Name, uint64_t *Size = nullptr);

private:
  std::unique_ptr<LIEF::MachO::FatBinary> Fat;
};

class LoadedDll : public LoadedLibrary {
public:
  HMODULE Ptr;

  uint64_t findSymbol(DynamicLoader &DL, const std::string &Name) override;
  bool hasUnderscorePrefix() override { return false; }
};

struct BinaryPath {
  std::string Path;
  bool Relative; // true iff `Path` is relative to install dir
};

struct AddrInfo {
  const std::string *LibPath;
  LoadedLibrary *Lib;
  std::string SymbolName;
};

class DynamicLoader {
public:
  DynamicLoader(uc_engine *UC);
  LoadedLibrary *load(const std::string &Path);
  void execute(LoadedLibrary *Lib);
  void *translate(void *Addr, va_list Args);

private:
  void error(const std::string &Msg, bool AppendLastError = false);
  bool canSegmentsSlide(LIEF::MachO::Binary &Bin);
  void mapMemory(uint64_t Addr, uint64_t Size, uc_prot Perms, void *Mem);
  BinaryPath resolvePath(const std::string &Path);
  LoadedLibrary *loadMachO(const std::string &Path);
  LoadedLibrary *loadPE(const std::string &Path);
  static constexpr uint64_t alignToPageSize(uint64_t Addr) {
    return Addr & (-PageSize);
  }
  static constexpr uint64_t roundToPageSize(uint64_t Addr) {
    return alignToPageSize(Addr + PageSize - 1);
  }
  template <typename... Args>
  void call(const std::string &Lib, const std::string &Func,
            Args &&... Params) {
    LoadedLibrary *L = load(Lib);
    uint64_t Addr = L->findSymbol(*this, Func);
    auto *Ptr = reinterpret_cast<void (*)(Args...)>(Addr);
    Ptr(std::forward<Args>(Params)...);
  }
  static bool catchFetchProtMem(uc_engine *UC, uc_mem_type Type, uint64_t Addr,
                                int Size, int64_t Value, void *Data);
  bool handleFetchProtMem(uc_mem_type Type, uint64_t Addr, int Size,
                          int64_t Value);
  static void catchCode(uc_engine *UC, uint64_t Addr, uint32_t Size,
                        void *Data);
  void handleCode(uint64_t Addr, uint32_t Size);
  static bool catchMemWrite(uc_engine *UC, uc_mem_type Type, uint64_t Addr,
                            int Size, int64_t Value, void *Data);
  bool handleMemWrite(uc_mem_type Type, uint64_t Addr, int Size, int64_t Value);
  // Finds only library, no symbol information is inspected. To do that, call
  // `inspect`.
  AddrInfo lookup(uint64_t Addr);
  AddrInfo inspect(uint64_t Addr);

  static constexpr int PageSize = 4096;
  static constexpr int R_SCATTERED = 0x80000000; // From `<mach-o/reloc.h>`.
  uc_engine *const UC;
  std::map<std::string, std::unique_ptr<LoadedLibrary>> LIs;
  uint64_t KernelAddr;
};
