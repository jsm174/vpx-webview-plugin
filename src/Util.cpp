// license:GPLv3+

#include "Plugin.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <cstdlib>
#include <dlfcn.h>
#endif

namespace WebView {

std::filesystem::path PluginDirectory()
{
#ifdef _WIN32
   HMODULE module = nullptr;
   if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&PluginDirectory), &module))
      return {};
   wchar_t buffer[MAX_PATH];
   const DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
   if (length == 0 || length >= MAX_PATH)
      return {};
   return std::filesystem::path(buffer).parent_path();
#else
   Dl_info info {};
   if (dladdr(reinterpret_cast<void*>(&PluginDirectory), &info) == 0 || !info.dli_fname)
      return {};
   char buffer[PATH_MAX];
   if (!realpath(info.dli_fname, buffer))
      return {};
   return std::filesystem::path(buffer).parent_path();
#endif
}

// CEF can only be initialized once per process while VPX loads and unloads plugins for every game, so the plugin keeps
// itself mapped and CEF alive for the lifetime of the process
void PinModule()
{
#ifdef _WIN32
   HMODULE module = nullptr;
   GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCSTR>(&PinModule), &module);
#else
   Dl_info info {};
   if (dladdr(reinterpret_cast<void*>(&PinModule), &info) != 0 && info.dli_fname)
      dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
#endif
}

std::string JsonEscape(const std::string& s)
{
   std::string out;
   out.reserve(s.size() + 2);
   out += '"';
   for (const unsigned char c : s)
   {
      switch (c)
      {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
         if (c < 0x20)
            out += std::format("\\u{:04x}", c);
         else
            out += static_cast<char>(c);
      }
   }
   out += '"';
   return out;
}

std::string VbsString(const std::string& s)
{
   std::string out = "\"";
   for (const char c : s)
   {
      if (c == '"')
         out += "\"\"";
      else if (static_cast<unsigned char>(c) < 0x20)
         out += std::format("\" & Chr({}) & \"", static_cast<int>(c));
      else
         out += c;
   }
   return out + "\"";
}

bool IsIdentifier(const std::string& name)
{
   if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0])))
      return false;
   return std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

}
