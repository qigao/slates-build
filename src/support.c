#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "build.h"

#include <core/process.h>
#include <core/scan.h>
#include <core/tlog.h>
#include <core/tstr.h>
#include <file/fs.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

typedef union Allocation Allocation;
union Allocation {
  Allocation *next;
  max_align_t alignment;
};
typedef struct JsonRoot {
  struct JsonRoot *next;
  Json *value;
} JsonRoot;

static Allocation *allocations;
static JsonRoot *json_roots;
static bool report_console_status;
static bool color_console_status;

void init_console_output(void) {
  report_console_status = same(environment_variable("SLATE_CONSOLE"), "1");
  color_console_status =
      report_console_status && !environment_variable("NO_COLOR");
#ifdef _WIN32
  if (color_console_status) {
    const DWORD outputs[] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (size_t i = 0; i < sizeof(outputs) / sizeof(*outputs); ++i) {
      const HANDLE handle = GetStdHandle(outputs[i]);
      DWORD mode;
      if (GetConsoleMode(handle, &mode))
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
  }
#endif
}

const char *console_mark(const char *text, const char *color) {
  return color_console_status ? format_string("\x1b[{}m{}\x1b[0m", color, text)
                              : text;
}

void console_start(const char *action, const Request *request) {
  if (!report_console_status)
    return;
  const char *prefix = console_mark("[Slate]", "1;36");
  if (request->targets.count == 1)
    print_line("{} {} {}", prefix, action, request->targets.items[0]);
  else
    print_line("{} {}", prefix, action);
}

int console_finish(const char *action, int status) {
  if (report_console_status)
    print_line("{} {} {} (exit {})",
               console_mark("[Slate]", status ? "31" : "32"), action,
               status ? "failed" : "succeeded", status);
  return status;
}

static void cleanup(void) {
  while (json_roots) {
    json_free(json_roots->value);
    json_roots = json_roots->next;
  }
  while (allocations) {
    Allocation *next = allocations->next;
    free(allocations);
    allocations = next;
  }
}

static void write_failure(vstr message) {
  fputs(color_console_status ? "\x1b[31m" SLATE_NAME ":\x1b[0m "
                            : SLATE_NAME ": ", stderr);
  if (message.len)
    fwrite(message.data, 1, message.len, stderr);
  fputc('\n', stderr);
}

static void failure_sink(const log_entry_t *entry, void *context) {
  write_failure(entry->message);
  *(bool *)context = true;
}

static void report_failure(vstr message) {
  bool reported = false;
  tlog_t *logger =
      tlog_create(&(tlog_config_t){.min_level = SALTS_LOG_LEVEL_ERROR});
  if (logger) {
    // Use a callback to preserve the CLI format and full diagnostic length.
    log_sink_t *sink = sink_callback_create(failure_sink, &reported);
    if (sink) {
      if (tlog_add_sink(logger, sink) == 0)
        log_str(logger, SALTS_LOG_LEVEL_ERROR, VSTR_LIT(SLATE_NAME),
                (vstr){0}, 0, message);
      else
        sink_destroy(sink);
    }
    // Join the sink worker before checking whether it delivered the message.
    tlog_destroy(logger);
  }
  if (!reported)
    write_failure(message);
}

_Noreturn void fail(const char *message) {
  report_failure(vstr_from_cstr(message));
  exit(EXIT_FAILURE);
}

_Noreturn void fail_formatted(tstr message) {
  if (!message)
    fail("could not allocate formatted diagnostic");
  report_failure(vstr_from_buf(message, tstr_len(message)));
  tstr_free(message);
  exit(EXIT_FAILURE);
}

void print_formatted_line(tstr line) {
  if (!line)
    fail("could not allocate formatted text");
  puts(line);
  tstr_free(line);
}

void *take(size_t size) {
  static bool initialized;
  if (!initialized) {
    if (atexit(cleanup))
      fail("cannot register cleanup");
    initialized = true;
  }
  if (size > SIZE_MAX - sizeof(Allocation))
    fail("allocation too large");
  Allocation *allocation = calloc(1, sizeof(*allocation) + size);
  if (!allocation)
    fail("out of memory");
  allocation->next = allocations;
  allocations = allocation;
  return allocation + 1;
}

void *reserve(void *old, size_t count, size_t *capacity, size_t width) {
  if (count <= *capacity)
    return old;
  size_t next = *capacity ? *capacity : 8;
  while (next < count) {
    if (next > SIZE_MAX / 2)
      fail("collection too large");
    next *= 2;
  }
  if (next > SIZE_MAX / width)
    fail("collection too large");
  void *result = take(next * width);
  if (old)
    memcpy(result, old, *capacity * width);
  *capacity = next;
  return result;
}

char *slice(const char *str, size_t length) {
  char *result = take(length + 1);
  memcpy(result, str, length);
  return result;
}

char *copy(const char *str) { return slice(str, strlen(str)); }

char *copy_formatted(tstr value) {
  if (!value)
    fail("could not allocate formatted text");
  char *result = slice(value, tstr_len(value));
  tstr_free(value);
  return result;
}

bool same(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
bool starts(const char *s, const char *prefix) {
  return tstr_starts_with(s, prefix) != 0;
}
bool ends(const char *s, const char *suffix) {
  return tstr_ends_with(s, suffix) != 0;
}
bool filename_matches(const char *pattern, const char *name) {
  const char *after_star = NULL, *restart = NULL;
  while (*name) {
    if (*pattern == '*') {
      after_star = ++pattern;
      restart = name;
    } else if (*pattern == *name) {
      ++pattern;
      ++name;
    } else if (after_star) {
      pattern = after_star;
      name = ++restart;
    } else {
      return false;
    }
  }
  while (*pattern == '*')
    ++pattern;
  return !*pattern;
}

bool identifier(const char *s) {
  if (!s || !*s || same(s, ".") || same(s, ".."))
    return false;
  for (; *s; ++s) {
    if (!isalnum((unsigned char)*s) && !strchr("_-.", *s))
      return false;
  }
  return true;
}

void push(Strings *list, const char *value) {
  list->items = reserve(list->items, list->count + 1, &list->capacity,
                        sizeof(*list->items));
  list->items[list->count++] = value;
}
bool contains(const Strings *list, const char *value) {
  for (size_t i = 0; i < list->count; ++i)
    if (same(list->items[i], value))
      return true;
  return false;
}
void unique(Strings *list, const char *value) {
  if (!contains(list, value))
    push(list, value);
}
static int compare_strings(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}
void sort_strings(Strings *list) {
  if (list->count > 1)
    qsort(list->items, list->count, sizeof(*list->items), compare_strings);
}

void append_n(Text *text, const char *value, size_t length) {
  if (length > SIZE_MAX - text->length - 1)
    fail("text too large");
  text->data =
      reserve(text->data, text->length + length + 1, &text->capacity, 1);
  memcpy(text->data + text->length, value, length);
  text->length += length;
  text->data[text->length] = 0;
}
void append(Text *text, const char *value) {
  append_n(text, value, strlen(value));
}
void append_formatted(Text *text, tstr value) {
  if (!value)
    fail("could not allocate formatted text");
  append_n(text, value, tstr_len(value));
  tstr_free(value);
}
const char *contents(const Text *text) { return text->data ? text->data : ""; }

const Setting *setting(const Settings *map, const char *key) {
  for (size_t i = 0; i < map->count; ++i)
    if (same(map->items[i].name, key))
      return &map->items[i];
  return NULL;
}
const Json *get(const Settings *map, const char *key) {
  const Setting *entry = setting(map, key);
  return entry ? entry->value : NULL;
}
void put(Settings *map, const char *key, const Json *value,
         const char *origin) {
  for (size_t i = 0; i < map->count; ++i) {
    if (same(map->items[i].name, key)) {
      map->items[i] = (Setting){key, value, origin};
      return;
    }
  }
  map->items =
      reserve(map->items, map->count + 1, &map->capacity, sizeof(*map->items));
  map->items[map->count++] = (Setting){key, value, origin};
}
void overlay(Settings *map, const Json *value, const char *origin) {
  if (!value)
    return;
  object(value, origin);
  for (size_t i = 0; i < json_object_size(value); ++i)
    put(map, json_object_key(value, i), json_object_value(value, i), origin);
}

Json *own(Json *value) {
  if (!value)
    fail("out of memory creating JSON");
  JsonRoot *root = take(sizeof(*root));
  *root = (JsonRoot){json_roots, value};
  json_roots = root;
  return value;
}
void json_set(Json *obj, const char *key, Json *value) {
  if (value && json_object_add_checked(obj, key, value))
    return;
  if (value)
    json_free(value);
  failf("cannot add JSON member {}", key);
}
void json_add(Json *arr, Json *value) {
  if (value && json_array_add_checked(arr, value))
    return;
  if (value)
    json_free(value);
  fail("cannot append JSON element");
}
const Json *member(const Json *value, const char *key) {
  return value ? json_object_get(value, key) : NULL;
}
const char *string(const Json *value, const char *where) {
  const char *result =
      value && json_type(value) == JSON_STRING ? json_string(value) : NULL;
  if (!result || strlen(result) != json_string_len(value))
    failf("{}: expected a string without NUL characters", where);
  return result;
}
const char *optional_string(const Json *value, const char *where) {
  return value ? string(value, where) : NULL;
}
void object(const Json *value, const char *where) {
  if (!value || json_type(value) != JSON_OBJECT)
    failf("{}: expected an object", where);
}
void array(const Json *value, const char *where) {
  if (!value || json_type(value) != JSON_ARRAY)
    failf("{}: expected an array", where);
}
void check_fields(const Json *value, const char *where, const char *fields) {
  object(value, where);
  for (size_t i = 0; i < json_object_size(value); ++i) {
    const char *key = json_object_key(value, i);
    if (!strstr(fields, format_string(" {} ", key)))
      failf("{}: unsupported field '{}'", where, key);
  }
}
void read_strings(Strings *out, const Json *value, const char *where) {
  if (!value)
    return;
  array(value, where);
  for (size_t i = 0; i < json_array_size(value); ++i)
    push(out, string(json_array_get(value, i), where));
}

static size_t skip_quoted(const char *data, size_t n, size_t i) {
  const char *end = data + n;
  const char *p = data + i + 1;
  while (p < end) {
    p = scan_to_any2(p, end, '"', '\\');
    if (p == end || *p == '"')
      break;
    ++p;
    if (p < end)
      ++p;
  }
  return (size_t)(p - data);
}

static void check_json_keys(const Json *value, const char *manifest,
                            unsigned depth) {
  if (depth > 128)
    failf("{}: manifest nesting is too deep", manifest);
  if (json_type(value) == JSON_OBJECT) {
    for (size_t i = 0; i < json_object_size(value); ++i) {
      const char *key = json_object_key(value, i);
      if (strlen(key) != json_object_key_len(value, i))
        failf("{}: object key contains a NUL", manifest);
      for (size_t j = 0; j < i; ++j)
        if (same(key, json_object_key(value, j)))
          failf("{}: duplicate key '{}'", manifest, key);
      check_json_keys(json_object_value(value, i), manifest, depth + 1);
    }
  } else if (json_type(value) == JSON_ARRAY) {
    for (size_t i = 0; i < json_array_size(value); ++i)
      check_json_keys(json_array_get(value, i), manifest, depth + 1);
  }
}

Json *load_json(const char *manifest) {
  size_t n;
  const char *raw = read_file(manifest, &n);
  if (n >= 3 && !memcmp(raw, "\xEF\xBB\xBF", 3)) {
    raw += 3;
    n -= 3;
  }
  if (memchr(raw, 0, n) || !vstr_utf8_valid(vstr_from_buf(raw, n)))
    failf("{}: expected UTF-8 JSON without NUL bytes", manifest);
  // Preserve byte offsets and line breaks while adapting JSONC to JsonParser.
  char *data = slice(raw, n);
  const char *end = data + n;
  for (size_t i = 0; i < n; ++i) {
    i = (size_t)(scan_to_any2(data + i, end, '"', '/') - data);
    if (i == n)
      break;
    if (data[i] == '"') {
      i = skip_quoted(data, n, i);
    } else if (i + 1 < n && data[i + 1] == '/') {
      size_t stop = (size_t)(scan_to_char(data + i + 2, end, '\n') - data);
      memset(data + i, ' ', stop - i);
      i = stop;
    } else if (i + 1 < n && data[i + 1] == '*') {
      const char *close = scan_mem(data + i + 2, n - i - 2, "*/", 2);
      if (!close)
        failf("{}: unterminated block comment", manifest);
      size_t last = (size_t)(close + 1 - data);
      for (; i < last; ++i)
        if (data[i] != '\n')
          data[i] = ' ';
      data[i] = ' ';
    }
  }
  for (size_t i = 0; i < n; ++i) {
    i = (size_t)(scan_to_any2(data + i, end, '"', ',') - data);
    if (i == n)
      break;
    if (data[i] == '"') {
      i = skip_quoted(data, n, i);
    } else if (data[i] == ',') {
      const char *next = scan_skip_sp_tab_cr_lf(data + i + 1, end);
      vstr before = vstr_trim_right(vstr_from_buf(data, i), " \t\r\n");
      if (before.len && !strchr("{[,", before.data[before.len - 1]) &&
          next < end && (*next == '}' || *next == ']'))
        data[i] = ' ';
    }
  }
  Json *value = json_parse(data, n);
  if (!value)
    failf("{}: {}", manifest, json_get_error());
  own(value);
  check_json_keys(value, manifest, 0);
  return value;
}

#ifdef _WIN32
static wchar_t *wide(const char *name) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
  if (!n)
    fail("invalid UTF-8 path");
  wchar_t *result = take((size_t)n * sizeof(*result));
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, result, n))
    fail("cannot convert UTF-8 path");
  return result;
}
static char *utf8(const wchar_t *name) {
  int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL);
  if (!n)
    fail("cannot convert Windows path");
  char *result = take((size_t)n);
  if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, result, n, NULL, NULL))
    fail("cannot convert Windows path");
  return result;
}
static char *slash(char *name) {
  const char *end = name + strlen(name);
  for (const char *sep = scan_char(name, end, '\\'); sep;
       sep = scan_char(sep + 1, end, '\\'))
    name[sep - name] = '/';
  return name;
}
#endif

void utf8_arguments(int *count, char ***args) {
#ifdef _WIN32
  wchar_t **parsed = CommandLineToArgvW(GetCommandLineW(), count);
  if (!parsed)
    fail("cannot parse Windows command line");
  char **converted = take(((size_t)*count + 1) * sizeof(*converted));
  for (int i = 0; i < *count; ++i)
    converted[i] = utf8(parsed[i]);
  LocalFree(parsed);
  *args = converted;
#else
  (void)count;
  (void)args;
#endif
}

const char *environment_variable(const char *name) {
#ifdef _WIN32
  wchar_t *key = wide(name);
  DWORD capacity = GetEnvironmentVariableW(key, NULL, 0);
  if (!capacity)
    return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? NULL : "";
  wchar_t *value = take((size_t)capacity * sizeof(*value));
  if (GetEnvironmentVariableW(key, value, capacity) >= capacity)
    failf("environment value for {} changed while being read", name);
  return utf8(value);
#else
  return getenv(name);
#endif
}

const char *native_tool_text(const char *value) {
#ifdef _WIN32
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0)) {
    const int count = MultiByteToWideChar(CP_ACP, 0, value, -1, NULL, 0);
    if (!count)
      fail("cannot decode native tool output");
    wchar_t *buffer = take((size_t)count * sizeof(*buffer));
    if (!MultiByteToWideChar(CP_ACP, 0, value, -1, buffer, count))
      fail("cannot decode native tool output");
    return utf8(buffer);
  }
#endif
  return value;
}

const char *path(const char *base, const char *name) {
  if (!*name)
    return base;
#ifdef _WIN32
  // Rooted paths and drive-relative paths use Windows' own resolution rules.
  const bool relative = name[0] != '/' && name[0] != '\\' &&
                        !(isalpha((unsigned char)name[0]) && name[1] == ':');
#else
  const bool relative = !fs_path_is_absolute(name);
#endif
  if (relative) {
    const char *directory = *base ? base : "/";
    size_t directory_length = strlen(directory), name_length = strlen(name);
    if (name_length > SIZE_MAX - 2 ||
        directory_length > SIZE_MAX - name_length - 2)
      fail("path too large");
    size_t capacity = directory_length + name_length + 2;
    char *joined = take(capacity);
    int rc = fs_path_join(joined, capacity, directory, name);
    if (rc)
      failf("cannot join '{}' and '{}' ({})", directory, name, rc);
    name = joined;
  }
#ifdef _WIN32
  wchar_t *input = wide(name);
  DWORD length = GetFullPathNameW(input, 0, NULL, NULL);
  if (!length)
    failf("cannot resolve path '{}'", name);
  wchar_t *result = take((size_t)length * sizeof(*result));
  if (!GetFullPathNameW(input, length, result, NULL))
    failf("cannot resolve '{}'", name);
  return slash(utf8(result));
#else
  char *result = take(strlen(name) + 2);
  size_t used = 1;
  result[0] = '/';
  for (const char *start = name; *start;) {
    if (*start == '/') {
      ++start;
      continue;
    }
    const char *end = strchr(start, '/');
    if (!end)
      end = start + strlen(start);
    size_t size = (size_t)(end - start);
    if (size == 2 && !strncmp(start, "..", 2)) {
      while (used > 1 && result[used - 1] != '/')
        --used;
      if (used > 1)
        --used;
    } else if (!(size == 1 && *start == '.')) {
      if (used > 1)
        result[used++] = '/';
      memcpy(result + used, start, size);
      used += size;
    }
    start = end;
  }
  result[used] = 0;
  return result;
#endif
}

const char *parent(const char *name) {
  size_t capacity = strlen(name) + 2;
  char *directory = take(capacity);
  int rc = fs_path_dirname(name, directory, capacity);
  if (rc)
    failf("cannot get parent of '{}' ({})", name, rc);
#ifdef _WIN32
  // Core returns "C:" for a direct child of "C:/".
  if (directory[0] && directory[1] == ':' && !directory[2]) {
    directory[2] = '/';
    directory[3] = '\0';
  }
#endif
  return directory;
}
const char *filename(const char *name) {
  if (!*name)
    return name;
  size_t capacity = strlen(name) + 1;
  char *result = take(capacity);
  int rc = fs_path_basename(name, result, capacity);
  if (rc)
    failf("cannot get filename of '{}' ({})", name, rc);
  return result;
}
bool inside(const char *directory, const char *file) {
  size_t n = strlen(directory);
  if (strlen(file) < n)
    return false;
#ifdef _WIN32
  if (_strnicmp(directory, file, n))
    return false;
#else
  if (strncmp(directory, file, n))
    return false;
#endif
  return !file[n] || file[n] == '/' || directory[n - 1] == '/';
}

const char *cwd(void) {
#ifdef _WIN32
  DWORD n = GetCurrentDirectoryW(0, NULL);
  if (!n)
    fail("cannot get working directory");
  wchar_t *name = take((size_t)n * sizeof(*name));
  if (!GetCurrentDirectoryW(n, name))
    fail("cannot get working directory");
  return slash(utf8(name));
#else
  size_t n = 4096;
  for (;;) {
    char *dir = take(n);
    if (getcwd(dir, n))
      return dir;
    if (errno != ERANGE)
      failf("cannot get working directory: {}", strerror(errno));
    n *= 2;
  }
#endif
}

const char *executable_path(void) {
#ifdef _WIN32
  wchar_t *name = take(32768 * sizeof(*name));
  DWORD n = GetModuleFileNameW(NULL, name, 32768);
  if (!n || n >= 32768)
    fail("cannot locate " SLATE_NAME " executable");
  return slash(utf8(name));
#elif defined(__APPLE__)
  uint32_t n = 0;
  (void)_NSGetExecutablePath(NULL, &n);
  char *name = take(n);
  if (_NSGetExecutablePath(name, &n))
    fail("cannot locate " SLATE_NAME " executable");
  return path(cwd(), name);
#else
  size_t n = 4096;
  for (;;) {
    char *name = take(n + 1);
    ssize_t count = readlink("/proc/self/exe", name, n);
    if (count < 0)
      fail("cannot locate " SLATE_NAME " executable");
    if ((size_t)count < n)
      return name;
    n *= 2;
  }
#endif
}

static bool needs_utf8_filesystem(const char *name) {
#ifdef _WIN32
  for (const unsigned char *byte = (const unsigned char *)name; *byte; ++byte)
    if (*byte >= 0x80)
      return true;
#else
  (void)name;
#endif
  return false;
}

static int stat_path(const char *name, fs_stat_t *info, bool nofollow) {
  if (!needs_utf8_filesystem(name))
    return nofollow ? fs_lstat(name, info) : fs_stat(name, info);
  fs_root_t *directory;
  int rc = fs_root_open(parent(name), &directory);
  if (rc)
    return rc;
  rc = nofollow ? fs_root_lstat(directory, filename(name), info)
                : fs_root_stat(directory, filename(name), info);
  fs_root_close(directory);
  return rc;
}

bool is_file(const char *name) {
  fs_stat_t info;
  return stat_path(name, &info, false) == 0 && info.is_file;
}
bool is_dir(const char *name) {
  fs_stat_t info;
  return stat_path(name, &info, false) == 0 && info.is_directory;
}
bool is_link(const char *name) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(wide(name));
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  fs_stat_t info;
  return stat_path(name, &info, true) == 0 && info.is_symlink;
#endif
}

#ifdef _WIN32
static void list_utf8_dir(const char *directory, Strings *entries) {
  fs_root_t *root;
  int rc = fs_root_open(directory, &root);
  if (rc)
    failf("cannot open directory '{}' ({})", directory, rc);
  fs_root_dir_t *reader;
  rc = fs_root_opendir_self(root, &reader);
  if (rc) {
    fs_root_close(root);
    failf("cannot enumerate '{}' ({})", directory, rc);
  }
  uint64_t cookie = 0;
  char name[SALTS_FS_ROOT_DIRENT_NAME_MAX + 1];
  fs_root_dirent_t entry;
  while ((rc = fs_root_readdir(reader, cookie, name, sizeof(name), &entry)) >
         0) {
    cookie = entry.next_cookie;
    if (entry.type != SALTS_FS_DIRENT_SYMLINK && !same(name, ".") &&
        !same(name, ".."))
      push(entries, copy(name));
  }
  fs_root_closedir(reader);
  fs_root_close(root);
  if (rc < 0)
    failf("cannot enumerate '{}' ({})", directory, rc);
  sort_strings(entries);
}
#endif

void list_dir(const char *directory, Strings *entries) {
#ifdef _WIN32
  list_utf8_dir(directory, entries);
#else
  fs_dir_t *reader = NULL;
  int rc = fs_opendir(directory, &reader);
  if (rc)
    failf("cannot read directory '{}' ({})", directory, rc);
  fs_dirent_t entry;
  while ((rc = fs_readdir(reader, &entry)) > 0) {
    if (entry.type == SALTS_FS_DIRENT_SYMLINK)
      continue;
    if (entry.type == SALTS_FS_DIRENT_UNKNOWN) {
      fs_stat_t info;
      if (stat_path(path(directory, entry.name), &info, true) ||
          info.is_symlink)
        continue;
    }
    push(entries, copy(entry.name));
  }
  int closed = fs_closedir(reader);
  if (rc < 0 || closed)
    failf("cannot enumerate '{}' ({})", directory, rc ? rc : closed);
  sort_strings(entries);
#endif
}

static bool executable(const char *name) {
  if (needs_utf8_filesystem(name))
    return is_file(name);
  return is_file(name) && fs_access(name, SALTS_FS_ACCESS_EXEC) == 0;
}
const char *find_program(const char *root, const char *name) {
  if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) {
    const char *absolute = path(root, name);
    return executable(absolute) ? absolute : NULL;
  }
#ifdef _WIN32
  const char separator = ';';
  if (!strchr(name, '.'))
    name = format_string("{}.exe", name);
#else
  const char separator = ':';
#endif
  const char *search = environment_variable("PATH");
  while (search && *search) {
    const char *end = strchr(search, separator);
    if (!end)
      end = search + strlen(search);
    if (end != search) {
      const char *dir = slice(search, (size_t)(end - search));
      const char *candidate = path(path(root, dir), name);
      if (executable(candidate))
        return candidate;
    }
    search = *end ? end + 1 : end;
  }
  return NULL;
}

static const char *capture_process(const process_options_t *options,
                                   bool allow_nonzero, Text output[2]) {
  process_t *child;
  int rc = process_spawn(options, &child);
  if (rc)
    return format_string("cannot run {} ({})", options->program, rc);
  process_result_t result;
  rc = process_wait(child, &result);
  for (int stream = 0; stream < 2 && !rc; ++stream) {
    size_t count;
    char buffer[4096];
    for (;;) {
      int status =
          stream ? process_read_stderr(child, buffer, sizeof(buffer), &count)
                 : process_read_stdout(child, buffer, sizeof(buffer), &count);
      if (status != SALTS_OK || !count)
        break;
      append_n(&output[stream], buffer, count);
    }
  }
  process_destroy(child);
  if (rc)
    return format_string("waiting for {} failed ({})", options->program, rc);
  if (result.state != SALTS_PROCESS_EXITED ||
      (!allow_nonzero && result.exit_code != 0))
    return format_string("{} exited with state {} and status {}{}{}",
                         options->program, process_state_name(result.state),
                         result.exit_code, output[1].length ? ": " : "",
                         contents(&output[1]));
  return NULL;
}

const char *capture_program(const char *program, const char *const *args,
                            const char *directory, const char *const *env,
                            bool allow_nonzero, const char **out,
                            const char **err) {
  process_options_t options;
  process_options_init(&options);
  options.program = program;
  options.args = args;
  options.cwd = directory;
  options.env = env;
  options.timeout_ms = 10000;
  options.max_output_bytes = 65536;
  Text output[2] = {0};
  const char *problem = capture_process(&options, allow_nonzero, output);
  *out = contents(&output[0]);
  *err = contents(&output[1]);
  return problem;
}

#ifdef _WIN32
int write_vsdevcmd_environment(void) {
  if (!SetEnvironmentVariableW(L"_SLATE_VSDEV_SCRIPT", NULL) ||
      !SetEnvironmentVariableW(L"_SLATE_VSDEV_EXE", NULL))
    return EXIT_FAILURE;
  wchar_t *block = GetEnvironmentStringsW();
  if (!block)
    return EXIT_FAILURE;
  const wchar_t *end = block;
  while (*end)
    end += wcslen(end) + 1;
  const size_t count = end == block ? 2 : (size_t)(end - block) + 1;
  const bool written = _setmode(_fileno(stdout), _O_BINARY) != -1 &&
                       fwrite(block, sizeof(*block), count, stdout) == count &&
                       fflush(stdout) == 0;
  FreeEnvironmentStringsW(block);
  return written ? EXIT_SUCCESS : EXIT_FAILURE;
}

void initialize_vsdevcmd(const char *root, const char *script,
                         const char *arch) {
  const char *host = host_arch();
  if ((!same(arch, "x86") && !same(arch, "x64") && !same(arch, "arm64")) ||
      (!same(host, "x86") && !same(host, "x64") && !same(host, "arm64")))
    failf("unsupported MSVC architecture {}/{}", host, arch);
  // Expand each path once inside quotes. CALL would expand batch paths again.
  const char *env[] = {format_string("_SLATE_VSDEV_SCRIPT={}", script),
                       format_string("_SLATE_VSDEV_EXE={}", executable_path()),
                       NULL};
  process_options_t options;
  process_options_init(&options);
  options.flags |= SALTS_PROCESS_SHELL_COMMAND;
  options.program =
      format_string("\"%_SLATE_VSDEV_SCRIPT%\" -arch={} -host_arch={} 1>&2 && "
                    "\"%_SLATE_VSDEV_EXE%\" _vsenv",
                    arch, host);
  options.cwd = root;
  options.env = env;
  options.timeout_ms = 30000;
  options.max_output_bytes = 1024 * 1024;
  Text output[2] = {0};
  const char *problem = capture_process(&options, false, output);
  if (problem)
    failf("cannot initialize MSVC with '{}': {}", script, problem);

  const size_t length = output[0].length;
  const size_t count = length / sizeof(wchar_t);
  if (length % sizeof(wchar_t) || count < 2)
    failf("'{}' produced an invalid MSVC environment", script);
  wchar_t *block = take(length);
  memcpy(block, output[0].data, length);
  if (block[count - 1] || block[count - 2] || !SetEnvironmentStringsW(block))
    failf("cannot import the MSVC environment from '{}'", script);
  if (!same(environment_variable("VSCMD_ARG_TGT_ARCH"), arch))
    failf("'{}' did not initialize MSVC for {}", script, arch);
}
#endif

const char *probe_program(const char *program, const char *const *args,
                          const char *directory, bool allow_nonzero,
                          const char **reported) {
  const char *out, *err;
  const char *problem = capture_program(program, args, directory, NULL,
                                         allow_nonzero, &out, &err);
  if (problem)
    return problem;
  vstr line = vstr_from_cstr(*out ? out : err);
  line = vstr_trim_left(line, "\r\n ");
  line = vstr_split_next(&line, vstr_from_cstr("\n"));
  line = vstr_trim(line, "\r ");
  *reported = line.len ? slice(line.data, line.len) : "";
  return NULL;
}

const char *read_file(const char *name, size_t *length) {
  if (needs_utf8_filesystem(name)) {
    fs_root_t *directory;
    int rc = fs_root_open(parent(name), &directory);
    if (rc)
      failf("cannot open parent of '{}' ({})", name, rc);
    fs_root_file_t *file;
    rc = fs_root_file_open(directory, filename(name), SALTS_FS_O_RDONLY, 0,
                           &file);
    fs_root_close(directory);
    if (rc)
      failf("cannot open '{}' ({})", name, rc);
    char buffer[8192];
    Text text = {0};
    while ((rc = fs_root_file_read(file, buffer, sizeof(buffer))) > 0)
      append_n(&text, buffer, (size_t)rc);
    int closed = fs_root_file_close(file);
    if (rc < 0 || closed)
      failf("cannot read '{}' ({})", name, rc ? rc : closed);
    *length = text.length;
    return contents(&text);
  }
  fs_buf_t buffer;
  int rc = fs_read_file(name, &buffer);
  if (rc)
    failf("cannot read '{}' ({})", name, rc);
  const char *data = buffer.len ? slice(buffer.base, buffer.len) : "";
  *length = buffer.len;
  fs_buf_free(&buffer);
  return data;
}
void mkdirs(const char *name) {
  if (is_dir(name))
    return;
  const char *base = parent(name);
  if (same(name, base))
    failf("cannot locate directory root for '{}'", name);
  mkdirs(base);
  int rc;
  if (needs_utf8_filesystem(name)) {
    fs_root_t *directory;
    rc = fs_root_open(base, &directory);
    if (!rc) {
      rc = fs_root_mkdir(directory, filename(name), 0777);
      fs_root_close(directory);
    }
  } else {
    rc = fs_mkdir(name, 0777);
  }
  if (rc && !is_dir(name))
    failf("cannot create directory '{}' ({})", name, rc);
}
int remove_file(const char *name) {
  if (!needs_utf8_filesystem(name))
    return fs_unlink(name);
  fs_root_t *directory;
  int rc = fs_root_open(parent(name), &directory);
  if (rc)
    return rc;
  rc = fs_root_unlink(directory, filename(name));
  fs_root_close(directory);
  return rc;
}

static int publish_utf8(const char *name, const char *temp, const char *data,
                        size_t length) {
  fs_root_t *directory;
  int rc = fs_root_open(parent(name), &directory);
  if (rc)
    return rc;
  fs_root_file_t *file;
  rc = fs_root_file_open(
      directory, filename(temp),
      SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_O_TRUNC, 0666, &file);
  if (!rc) {
    for (size_t offset = 0; offset < length;) {
      size_t chunk = length - offset;
      if (chunk > 65536)
        chunk = 65536;
      rc = fs_root_file_write(file, data + offset, chunk);
      if (rc <= 0) {
        if (!rc)
          rc = SALTS_EIO;
        break;
      }
      offset += (size_t)rc;
      rc = 0;
    }
    int closed = fs_root_file_close(file);
    if (!rc)
      rc = closed;
  }
  if (!rc)
    rc = fs_root_rename(directory, filename(temp), directory, filename(name));
  if (rc)
    fs_root_unlink(directory, filename(temp));
  fs_root_close(directory);
  return rc;
}

static const char *temporary_path(const char *name) {
#ifdef _WIN32
  return format_string("{}.tmp.{}", name, GetCurrentProcessId());
#else
  return format_string("{}.tmp.{}", name, (long)getpid());
#endif
}

void write_file(const char *name, const char *data, size_t length) {
  if (is_file(name)) {
    size_t old_length;
    const char *old = read_file(name, &old_length);
    if (length == old_length && !memcmp(old, data, length))
      return;
  }
  mkdirs(parent(name));
  const char *temp = temporary_path(name);
  if (needs_utf8_filesystem(name)) {
    int rc = publish_utf8(name, temp, data, length);
    if (rc)
      failf("cannot publish '{}' ({})", name, rc);
    return;
  }
  fs_buf_t buffer = fs_buf_init((char *)data, length);
  int rc = fs_write_file(temp, &buffer);
  if (!rc)
    rc = fs_rename(temp, name);
  if (rc) {
    fs_unlink(temp);
    failf("cannot publish '{}' ({})", name, rc);
  }
}
void write_json(const char *name, const Json *json) {
  size_t n;
  char *data = json_serialize_pretty(json, &n);
  if (!data)
    failf("cannot serialize '{}'", name);
  Text text = {0};
  append_n(&text, data, n);
  json_serialize_free(data);
  append(&text, "\n");
  write_file(name, contents(&text), text.length);
}
void copy_file(const char *source, const char *destination) {
  size_t size;
  const char *data = read_file(source, &size);
  write_file(destination, data, size);
}

void copy_install_file(const char *source, const char *destination) {
  mkdirs(parent(destination));
  const char *temp = temporary_path(destination);
#ifdef _WIN32
  const wchar_t *temporary = wide(temp);
  if (!CopyFileW(wide(source), temporary, TRUE))
    failf("cannot copy install source '{}' ({})", source, GetLastError());
  if (!MoveFileExW(temporary, wide(destination), MOVEFILE_REPLACE_EXISTING)) {
    const DWORD error = GetLastError();
    DeleteFileW(temporary);
    failf("cannot install '{}' ({})", destination, error);
  }
#else
  fs_stat_t info;
  fs_buf_t data;
  int rc = fs_stat(source, &info);
  if (!rc)
    rc = fs_read_file(source, &data);
  if (rc)
    failf("cannot read install source '{}' ({})", source, rc);
  rc = fs_write_file(temp, &data);
  fs_buf_free(&data);
  if (!rc)
    rc = fs_chmod(temp, info.mode & 0777);
  if (!rc)
    rc = fs_rename(temp, destination);
  if (rc) {
    fs_unlink(temp);
    failf("cannot install '{}' ({})", destination, rc);
  }
#endif
}
uint64_t path_stamp(const char *name) {
  fs_stat_t info;
  return stat_path(name, &info, false) == 0 ? info.mtime : 0;
}
const char *host_os(void) {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  fail("unsupported host OS");
#endif
}
const char *host_arch(void) {
#ifdef _WIN32
  SYSTEM_INFO info;
  GetNativeSystemInfo(&info);
  switch (info.wProcessorArchitecture) {
  case PROCESSOR_ARCHITECTURE_AMD64:
    return "x64";
  case PROCESSOR_ARCHITECTURE_ARM64:
    return "arm64";
  case PROCESSOR_ARCHITECTURE_INTEL:
    return "x86";
  }
#else
  struct utsname info;
  if (uname(&info))
    fail("cannot detect host architecture");
  if (same(info.machine, "x86_64") || same(info.machine, "amd64"))
    return "x64";
  if (same(info.machine, "aarch64") || same(info.machine, "arm64"))
    return "arm64";
  if (same(info.machine, "i686") || same(info.machine, "i386"))
    return "x86";
  if (same(info.machine, "riscv64"))
    return "riscv64";
#endif
  fail("unsupported host architecture");
}

const char *argument(const char *str) {
  Text text = {0};
#ifdef _WIN32
  append(&text, "\"");
  for (const char *start = str;;) {
    const char *end = start;
    while (*end == '\\')
      ++end;
    size_t count = (size_t)(end - start);
    if (*end == '"' || !*end)
      count *= 2;
    for (size_t i = 0; i < count; ++i)
      append(&text, "\\");
    if (!*end)
      break;
    if (*end == '"')
      append(&text, "\\");
    append_n(&text, end, 1);
    start = end + 1;
  }
  append(&text, "\"");
#else
  append(&text, "'");
  for (const char *c = str; *c; ++c) {
    if (*c == '\'')
      append(&text, "'\\''");
    else
      append_n(&text, c, 1);
  }
  append(&text, "'");
#endif
  return contents(&text);
}
const char *command(const Strings *args) {
  Text text = {0};
  for (size_t i = 0; i < args->count; ++i) {
    if (i)
      append(&text, " ");
    append(&text, argument(args->items[i]));
  }
  return contents(&text);
}
static const char *escape_ninja(const char *str, bool file) {
  Text text = {0};
  for (const char *c = str; *c; ++c) {
    if (*c == '\n' || *c == '\r' || (file && (*c == '|' || *c == '\t')))
      failf("unsupported character in Ninja {}", file ? "path" : "value");
    if (*c == '$' || (file && (*c == ' ' || *c == ':')))
      append(&text, "$");
    append_n(&text, c, 1);
  }
  return contents(&text);
}
const char *ninja_path(const char *str) { return escape_ninja(str, true); }
const char *ninja_value(const char *str) { return escape_ninja(str, false); }
