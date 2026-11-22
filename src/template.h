#ifndef SLATE_TEMPLATE_H
#define SLATE_TEMPLATE_H

#include "build.h"

#include <cmeta/data_reflect.h>
#include <jinja_cmeta.h>

struct BuildTemplate {
  const char *file;
  vstr source;
  JINJA_CMETA_TEMPLATE *compiled;
};

BuildTemplate read_build_template(const Project *project, const char *name);
JINJA_CMETA_ENV *create_template_env(void);
void compile_build_template(JINJA_CMETA_ENV *env, BuildTemplate *templ);
Text render_template(const BuildTemplate *templ,
                     const cmeta_data_desc *descriptor, const void *data,
                     const JINJA_CMETA_RENDER_OPTIONS *limits);
void load_artifact_names(Graph *graph);
void emit_sdk(const Graph *graph, const Module *module,
              const BuildTemplate *json, const BuildTemplate *pc,
              const char *manifest, const char *pkgconfig);

#endif
