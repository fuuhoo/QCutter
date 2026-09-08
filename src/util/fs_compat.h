// Compatibility header for <filesystem>.
//
// MSVC older than 19.23 / MinGW older than 9 / libstdc++ < 9 need
// <experimental/filesystem>. Modern toolchains (MSVC 19.5+, libstdc++ 9+,
// libc++) expose std::filesystem directly. Use whichever is available.
#if defined(__cpp_lib_filesystem) || \
    (defined(_MSC_FULL_VER) && _MSC_FULL_VER >= 192329913) || \
    (defined(__GNUC__) && (__GNUC__ > 9 || (__GNUC__ == 9 && __GNUC_MINOR__ >= 1)))
#  include <filesystem>
   namespace fs = std::filesystem;
#elif defined(_MSC_VER) && _MSC_VER >= 1900
#  include <filesystem>
   namespace fs = std::filesystem;
#else
#  include <experimental/filesystem>
   namespace fs = std::experimental::filesystem;
#endif