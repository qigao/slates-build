#ifndef SLATE_MODULE_H
#define SLATE_MODULE_H

#include "build.h"

void load_modules(Graph *graph);
const char *module_text(Project *project, const Module *module,
                        const char *text);
const char *module_path(Project *project, const Module *module,
                        const char *name);
void read_module_paths(Project *project, const Module *module, Strings *paths,
                       const Json *value);
void read_usage(Project *project, const Module *module, Usage *usage,
                Strings *deps, const Json *value, const char *origin);

#endif
