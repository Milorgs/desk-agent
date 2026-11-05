#ifndef DESK_PLATFORM_HPP
#define DESK_PLATFORM_HPP
#include <filesystem>
#include <string>
#include <string_view>
namespace fs=std::filesystem;
inline fs::path fs_path(std::string_view text){return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()),text.size()));}
inline std::string path_text(const fs::path &path){auto text=path.generic_u8string();return std::string(reinterpret_cast<const char *>(text.data()),text.size());}
std::string config_base();
std::string state_base();
char *canonical_directory(const char *path,char *out);
bool path_inside(const fs::path &root,const fs::path &path);
bool path_link(const fs::path &path);
void platform_initialize();
int terminal_columns();
bool terminal_supports_ansi();
bool terminal_supports_color();
const char *env_value(const char *name);
#ifdef _WIN32
#ifndef PATH_MAX
#define PATH_MAX 32768
#endif
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#include <io.h>
#include <cstdio>
#define isatty _isatty
#define strdup _strdup
#define strtok_r strtok_s
int asprintf(char **result,const char *format,...);
std::wstring wide(std::string_view value);
std::string utf8(std::wstring_view value);
#endif
#endif
