#include "template.h"
#include "template_paths.h"

#include <string.h>

BuildTemplate read_build_template(const Project *p, const char *name) {
  const char *override =
      optional_string(get(&p->fields, "templateDir"), "templateDir");
  const char *bin = parent(executable_path());
  const char *directories[] = {
      path(p->root, override ? override : PROJECT_TEMPLATE_DIR),
      path(bin, BUILD_TEMPLATE_DIR),
      path(bin, INSTALLED_TEMPLATE_PATH),
  };
  for (size_t i = 0; i < sizeof(directories) / sizeof(*directories); ++i) {
    const char *file = path(directories[i], name);
    if (is_file(file)) {
      size_t size;
      const char *source = read_file(file, &size);
      return (BuildTemplate){.file = file,
                             .source = vstr_from_buf(source, size)};
    }
  }
  failf("could not find the project or installed {} template", name);
}

JINJA_CMETA_ENV *create_template_env(void) {
  JINJA_CMETA_ENV_OPTIONS options = JINJA_CMETA_ENV_OPTIONS_INIT;
  options.compile.trim_blocks = 1;
  options.compile.keep_trailing_newline = 1;
  options.undefined_policy = JINJA_CMETA_UNDEFINED_STRICT;
  JINJA_CMETA_ERROR error = JINJA_CMETA_ERROR_INIT;
  JINJA_CMETA_ENV *env = jinja_cmeta_env_create(&options, &error);
  if (!env)
    failf("cannot initialize Jinja: {}", error.message);
  return env;
}

void compile_build_template(JINJA_CMETA_ENV *env, BuildTemplate *templ) {
  JINJA_CMETA_ERROR error = JINJA_CMETA_ERROR_INIT;
  templ->compiled = jinja_cmeta_env_compile(env, vstr_from_cstr(templ->file),
                                            templ->source, &error);
  if (!templ->compiled)
    failf("{}:{}: {}", templ->file, error.offset, error.message);
}

static int render_write(const char *text, size_t size, void *output) {
  append_n(output, text, size);
  return 0;
}

Text render_template(const BuildTemplate *templ,
                     const cmeta_data_desc *descriptor, const void *data,
                     const JINJA_CMETA_RENDER_OPTIONS *limits) {
  Text output = {0};
  const JINJA_CMETA_RENDERER sink = {render_write};
  JINJA_CMETA_ERROR error = JINJA_CMETA_ERROR_INIT;
  if (jinja_cmeta_render(templ->compiled, descriptor, data, limits, &sink,
                         &output, &error) != JINJA_CMETA_OK)
    failf("{}:{}: {}", templ->file, error.offset, error.message);
  return output;
}

Struct(ArtifactContext, (vstr, os), (vstr, driver), (vstr, arch),
       (vstr, config));

static const char *artifact_name_part(const Json *data, const char *key,
                                      const char *file) {
  const char *where = format_string("{}.{}", file, key);
  const char *value = string(member(data, key), where);
  if (*value && !identifier(value))
    failf("{}: invalid filename fragment '{}'", where, value);
  return value;
}

static ArtifactNames render_artifact_names(const BuildTemplate *templ,
                                           const Project *p,
                                           const Toolchain *tools) {
  static const cmeta_type_identity identity =
      CMETA_TYPE_ID_ATOM_INIT("slate.artifact.context");
  static const cmeta_type_desc storage = {.name = "ArtifactContext",
                                          .size = sizeof(ArtifactContext),
                                          .align = _Alignof(ArtifactContext),
                                          .kind = CMETA_T_OBJECT,
                                          .identity = &identity};
  const cmeta_data_desc *str = jinja_cmeta_vstr_data();
  const cmeta_data_field_desc fields[] = {
      {"slate.artifact.context.os", "os", offsetof(ArtifactContext, os),
       str},
      {"slate.artifact.context.driver", "driver",
       offsetof(ArtifactContext, driver), str},
      {"slate.artifact.context.arch", "arch",
       offsetof(ArtifactContext, arch), str},
      {"slate.artifact.context.config", "config",
       offsetof(ArtifactContext, config), str},
  };
  const cmeta_data_struct_shape shape = {StructMeta(ArtifactContext), fields,
                                         sizeof(fields) / sizeof(*fields)};
  const cmeta_data_desc descriptor = {
      .struct_size = sizeof(descriptor),
      .abi_version = CMETA_DATA_DESC_ABI_VERSION,
      .stable_id = "slate.artifact.context.data",
      .display_name = "ArtifactContext",
      .kind = CMETA_DATA_STRUCT,
      .storage_type = &storage,
      .shape = &shape};
  const ArtifactContext context = {.os = vstr_from_cstr(tools->os),
                                   .driver = vstr_from_cstr(tools->driver),
                                   .arch = vstr_from_cstr(tools->arch),
                                   .config = vstr_from_cstr(p->config)};
  const Text rendered = render_template(templ, &descriptor, &context, NULL);
  Json *data = json_parse(contents(&rendered), rendered.length);
  if (!data)
    failf("{}: expected JSON artifact names: {}", templ->file,
          json_get_error());
  own(data);
  object(data, templ->file);
  const char *symbol_suffix =
      optional_string(member(data, "symbolsSuffix"), templ->file);
  if (!symbol_suffix)
    symbol_suffix = same(tools->os, "macos") || same(tools->os, "ios")
                        ? ".dSYM" : ".debug";
  if (strpbrk(symbol_suffix, "/\\:\r\n"))
    failf("{}: invalid symbolsSuffix '{}'", templ->file, symbol_suffix);
  ArtifactNames names = {
      .executable_suffix =
          artifact_name_part(data, "executableSuffix", templ->file),
      .static_prefix = artifact_name_part(data, "staticPrefix", templ->file),
      .static_suffix = artifact_name_part(data, "staticSuffix", templ->file),
      .shared_prefix = artifact_name_part(data, "sharedPrefix", templ->file),
      .shared_suffix = artifact_name_part(data, "sharedSuffix", templ->file),
      .import_prefix = artifact_name_part(data, "importPrefix", templ->file),
      .import_suffix = artifact_name_part(data, "importSuffix", templ->file),
      .object_suffix = artifact_name_part(data, "objectSuffix", templ->file),
      .response_suffix =
          artifact_name_part(data, "responseSuffix", templ->file),
      .depfile_suffix = artifact_name_part(data, "depfileSuffix", templ->file),
      .pdb_suffix = artifact_name_part(data, "pdbSuffix", templ->file),
      .symbols_suffix = symbol_suffix,
  };
  const char *sidecar = tools->msvc ? names.pdb_suffix : names.depfile_suffix;
  if (!*names.object_suffix || !*names.response_suffix || !*sidecar)
    failf("{}: object and active sidecar suffixes must be nonempty",
          templ->file);
  if (same(names.response_suffix, sidecar))
    failf("{}: response file and {} suffixes must differ", templ->file,
          tools->msvc ? "PDB" : "depfile");
  return names;
}

void load_artifact_names(Graph *g) {
  Project *p = g->project;
  BuildTemplate *templ = take(sizeof(*templ));
  *templ = read_build_template(p, "artifact-names.json.j2");
  JINJA_CMETA_ENV *env = create_template_env();
  compile_build_template(env, templ);
  p->target.artifacts = render_artifact_names(templ, p, &p->target);
  p->host.artifacts = render_artifact_names(templ, p, &p->host);
  jinja_cmeta_release(templ->compiled);
  jinja_cmeta_env_destroy(env);
  templ->compiled = NULL;
  g->artifact_template = templ;
}
