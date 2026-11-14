/* Distribution parsing is independent of selection. The target and checks
 * decide which embedded components to decode before install preflight. */
#include "mdpkg.h"
#include "installer-js.h"

#include <libxml/tree.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

const char *const hook_names[HOOK_COUNT] = {"preflight", "preinstall",
                                            "postinstall", "postflight"};

static int one_of(const char *s, const char *const *list) {
  for (; *list; list++)
    if (!strcmp(s, *list))
      return 1;
  return 0;
}

static void check_identifier(const char *s) {
  if (!*s || strlen(s) > 200 || !strcmp(s, ".") || !strcmp(s, ".."))
    die("invalid package identifier: %s", s);
  for (const char *p = s; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_'))
      die("invalid package identifier: %s", s);
}

static Bytes component_member(Component *c, const char *name, int required) {
  char *path = path_join(c->xar_dir, name);
  Bytes b = xar_member(c->xar, path, required);
  free(path);
  return b;
}

/* Bundle upgrade/relocation elements only matter when a bundle with the same
 * identifier is already installed somewhere on the volume. mdpkg refuses
 * reinstalls and has no bundle database to search, so they are accepted and
 * the payload is installed where it says. */
static const char *const packageinfo_elements[] = {
    "payload",           "scripts",  "bundle-version", "upgrade-bundle",
    "update-bundle",     "atomic-update-bundle",       "strict-identifier",
    "relocate",          "bundle",   "groups",         NULL};

/* <groups><group>ID</group>...</groups>: the package groups (pkgutil's
 * --groups) a component belongs to. */
static char **read_groups(xmlNode *groups) {
  size_t count = 0;
  char **out = xcalloc(1, sizeof(char *));
  for (xmlNode *n = xml_elements(groups); n; n = xml_next_element(n)) {
    if (!xml_is(n, "group"))
      die("unsupported PackageInfo groups element: %s", xml_name(n));
    if (count >= 64)
      die("package group limit");
    char *id = xml_text(n);
    check_identifier(id);
    out = xrealloc(out, count + 2, sizeof(char *));
    out[count++] = id;
    out[count] = NULL;
  }
  return out;
}

static void read_scripts(Component *c, xmlNode *scripts) {
  for (xmlNode *n = xml_elements(scripts); n;
       n = xml_next_element(n)) {
    int hook = -1;
    for (int i = 0; i < HOOK_COUNT; i++)
      if (xml_is(n, hook_names[i]))
        hook = i;
    if (hook < 0 || c->hooks[hook])
      die("unsupported or duplicate install script: %s", xml_name(n));
    char *file = xml_attr(n, "file", NULL);
    c->hooks[hook] = path_normalize(file, 0);
    free(file);
    Entry *e = entries_find(&c->scripts, c->hooks[hook]);
    if (!e || !S_ISREG(e->mode) || !(e->mode & 0111))
      die("install script missing or not executable: %s", c->hooks[hook]);
  }
}

/* REF is a directory inside the product archive, or, with -dist, a package
 * file in the package directory. */
static void load_component(Package *pkg, const char *ref,
                           const char *expected_id, int metadata_only) {
  Component c = {0};
  if (pkg->pkg_dir) {
    c.package_path = path_join(pkg->pkg_dir, ref);
    c.xar_dir = xstrdup("");
  } else {
    c.package_path = xstrdup(pkg->path);
    c.xar_dir = xstrdup(ref);
  }
  for (size_t i = 0; i < pkg->count; i++)
    if (!strcmp(pkg->components[i].package_path, c.package_path) &&
        !strcmp(pkg->components[i].xar_dir, c.xar_dir)) {
      free(c.package_path);
      free(c.xar_dir);
      return;
    }
  if (pkg->count >= 1024)
    die("component limit");
  if (pkg->pkg_dir) {
    c.xar = xcalloc(1, sizeof(Xar));
    xar_open(c.xar, c.package_path);
    if (xar_member(c.xar, "Distribution", 0).data)
      die("%s: a Distribution's pkg-ref must name a component package", ref);
  } else
    c.xar = &pkg->xar;
  xmlDoc *doc = xml_parse(component_member(&c, "PackageInfo", 1));
  xmlNode *info = xml_root(doc, "pkg-info");
  char *format = xml_attr(info, "format-version", NULL);
  if (strcmp(format, "2"))
    die("unsupported PackageInfo format-version: %s", format);
  free(format);
  c.identifier = xml_attr(info, "identifier", NULL);
  check_identifier(c.identifier);
  if (expected_id && strcmp(c.identifier, expected_id))
    die("Distribution names %s, but PackageInfo says %s", expected_id,
        c.identifier);
  for (size_t i = 0; i < pkg->count; i++)
    if (!strcmp(pkg->components[i].identifier, c.identifier))
      die("duplicate component identifier: %s", c.identifier);
  c.version = xml_attr(info, "version", NULL);
  char *location = xml_attr(info, "install-location", "/");
  c.location = path_normalize(location, 1);
  free(location);
  for (xmlNode *n = xml_elements(info); n;
       n = xml_next_element(n))
    if (!one_of(xml_name(n), packageinfo_elements))
      die("unsupported PackageInfo element: %s", xml_name(n));

  if (metadata_only) {
    c.metadata_only = 1;
    xml_free(doc);
    pkg->components = xrealloc(pkg->components, pkg->count + 1, sizeof(Component));
    pkg->components[pkg->count++] = c;
    return;
  }

  Bytes payload = component_member(&c, "Payload", 0),
        scripts = component_member(&c, "Scripts", 0);
  if (payload.data) {
    c.spool = xcalloc(1, sizeof(Spool));
    spool_open(c.spool);
    cpio_parse(payload, &c.payload, c.spool);
  }
  if (scripts.data)
    cpio_parse(scripts, &c.scripts, NULL);
  c.bom = component_member(&c, "Bom", 1);
  if (c.bom.len < 32 || memcmp(c.bom.data, "BOMStore", 8))
    die("invalid BOM");
  xmlNode *count = xml_child(info, "payload");
  if (count) {
    /* pkgbuild counts every payload entry; Apple's Software Update packages
     * count regular files only. Either is consistent. */
    char *n = xml_attr(count, "numberOfFiles", NULL);
    size_t files = 0;
    for (size_t i = 0; i < c.payload.count; i++)
      files += S_ISREG(c.payload.items[i].mode);
    uint64_t claimed = parse_uint(n, 10);
    if (claimed != c.payload.count && claimed != files)
      die("PackageInfo numberOfFiles does not match the payload");
    free(n);
  }
  read_scripts(&c, xml_child(info, "scripts"));
  c.groups = read_groups(xml_child(info, "groups"));
  xml_free(doc);

  for (size_t i = 0; i < c.payload.count; i++) {
    Entry *e = &c.payload.items[i];
    char *placed = path_join(c.location, e->path);
    free(e->path);
    e->path = placed;
    if (e->link)
      symlink_check_lexical(e->path, e->link);
  }
  pkg->components = xrealloc(pkg->components, pkg->count + 1, sizeof(Component));
  pkg->components[pkg->count++] = c;
}

/* "#Some%20Name.pkg" -> "Some Name.pkg". Anything but an embedded reference
 * (a URL, a file: path, a sibling file) is refused. */
static char *embedded_reference(const char *ref) {
  if (*ref != '#')
    die("external package reference: %s", ref);
  char *decoded = xcalloc(strlen(ref), 1), *out = decoded;
  for (const char *p = ref + 1; *p; p++) {
    if (*p == '%' && p[1] && p[2]) {
      char hex[3] = {p[1], p[2], 0};
      if (!(*out++ = (char)parse_uint(hex, 16)))
        die("invalid package reference: %s", ref);
      p += 2;
    } else
      *out++ = *p;
  }
  char *path = path_normalize(decoded, 0);
  free(decoded);
  if (!*path)
    die("empty package reference");
  return path;
}

/* With -dist, a pkg-ref names a package file beside the Distribution, as a
 * Software Update product's does. Only a plain file name is accepted. */
static char *sibling_reference(const char *ref) {
  if (!*ref || *ref == '#' || strchr(ref, '/') || strchr(ref, ':') ||
      !strcmp(ref, ".") || !strcmp(ref, ".."))
    die("pkg-ref is not a package file name: %s", ref);
  return xstrdup(ref);
}

static int static_boolean(xmlNode *n, const char *key) {
  char *v = xml_attr(n, key, "true");
  int result = !strcmp(v, "true");
  if (!result && strcmp(v, "false"))
    die("Distribution needs JavaScript to decide %s=\"%s\"", key, v);
  free(v);
  return result;
}

/* Presentation and static metadata, plus the supported JS entrypoints. */
static const char *const distribution_elements[] = {
    "installer-script", "installer-gui-script", "title", "options", "domains",
    "background", "background-darkAqua", "welcome", "readme", "license",
    "conclusion", "product", "choices-outline", "line", "choice", "pkg-ref",
    "bundle-version", "bundle", "must-close", "app", "script",
    "installation-check", "volume-check", "allowed-os-versions", "os-version",
    /* Software Update's tags and platforms are informational here;
     * localization is system.localizedString's table. */
    "tags", "tag", "platforms", "client", "server", "localization", "strings",
    NULL};
static const char *const expression_attributes[] = {
    "selected", "enabled", "visible", "active", NULL};
static const char *const initial_attributes[] = {
    "start_selected", "start_enabled", "start_visible", NULL};

static void expression_label(char *buf, size_t len, xmlNode *node,
                              const char *attribute) {
  char *id = xml_attr(node, "id", "");
  snprintf(buf, len, "%s[%s]/@%s", xml_name(node), id, attribute);
  free(id);
}

static void validate_distribution(Package *pkg, InstallerJS *js, xmlNode *n,
                                   int depth, int require_scripts) {
  if (depth > LIMIT_DEPTH)
    die("Distribution nesting limit");
  for (; n; n = xml_next_element(n)) {
    if (!one_of(xml_name(n), distribution_elements))
      die("unsupported Distribution element: %s", xml_name(n));
    for (xmlAttr *a = n->properties; a; a = a->next) {
      const char *key = (const char *)a->name;
      if (one_of(key, initial_attributes))
        (void)static_boolean(n, key);
      if (one_of(key, expression_attributes)) {
        char *source = xml_attr(n, key, NULL), label[512];
        expression_label(label, sizeof(label), n, key);
        installer_js_compile(js, source, label, 1);
        if (strcmp(source, "true") && strcmp(source, "false")) {
          if (!require_scripts)
            die("Distribution require-scripts=false requires literal %s", label);
          pkg->needs_js = 1;
        }
        free(source);
      }
      if (!strcmp(key, "onConclusionScript"))
        die("unsupported Distribution attribute: onConclusionScript");
    }
    if (xml_is(n, "script")) {
      if (xmlHasProp(n, BAD_CAST "src"))
        die("external Distribution script is unsupported");
      char *source = xml_text(n);
      installer_js_compile(js, source, "Distribution/script", 0);
      free(source);
      pkg->needs_js = 1;
    }
    if (xml_is(n, "installation-check") || xml_is(n, "volume-check")) {
      /* A volume-check may consist only of allowed-os-versions. */
      int versions_only = xml_is(n, "volume-check") &&
                          xml_child(n, "allowed-os-versions") &&
                          !xmlHasProp(n, BAD_CAST "script");
      if (!versions_only) {
        char *source = xml_attr(n, "script", NULL);
        installer_js_compile(js, source, xml_name(n), 0);
        free(source);
      }
      pkg->needs_js = 1;
    }
    if (xml_is(n, "localization"))
      for (xmlNode *c = xml_elements(n); c; c = xml_next_element(c))
        if (!xml_is(c, "strings"))
          die("unsupported localization element: %s", xml_name(c));
    /* Under a volume-check, the versions are enforced (installer-js.c); a
     * list anywhere else is not part of a volume check and is reported. */
    if (xml_is(n, "os-version") && !(n->parent && n->parent->parent &&
                                    xml_is(n->parent->parent, "volume-check"))) {
      /* Informational, like hostArchitectures: the root's OS is not the
       * host's, and the package's own installation-check is what enforces. */
      char *min = xml_attr(n, "min", ""), *before = xml_attr(n, "before", "");
      for (xmlAttr *a = n->properties; a; a = a->next) {
        const char *key = (const char *)a->name, *p;
        if (strcmp(key, "min") && strcmp(key, "before"))
          die("unsupported os-version attribute: %s", key);
        for (p = *key == 'm' ? min : before; *p; p++)
          if (!strchr("0123456789.", *p))
            die("os-version %s is not a dotted version: %s", key,
                *key == 'm' ? min : before);
      }
      if (!*min && !*before)
        die("os-version without min or before");
      size_t old = pkg->allowed_os_versions ? strlen(pkg->allowed_os_versions) : 0;
      size_t len = old + strlen(min) + strlen(before) + 16;
      pkg->allowed_os_versions = xrealloc(pkg->allowed_os_versions, len, 1);
      snprintf(pkg->allowed_os_versions + old, len - old, "%s%s%s%s%s%s",
               old ? ", " : "", *min ? ">= " : "", min,
               *min && *before ? " and " : "", *before ? "< " : "", before);
      free(min);
      free(before);
    }
    if (xml_is(n, "options") && !pkg->host_architectures) {
      char *arch = xml_attr(n, "hostArchitectures", "");
      if (*arch)
        pkg->host_architectures = arch;
      else
        free(arch);
    }
    validate_distribution(pkg, js, xml_elements(n), depth + 1, require_scripts);
  }
}

static xmlNode *find_choice(xmlNode *dist, const char *id) {
  xmlNode *found = NULL;
  for (xmlNode *n = xml_elements(dist); n; n = xml_next_element(n)) {
    if (!xml_is(n, "choice"))
      continue;
    char *cid = xml_attr(n, "id", NULL);
    if (!strcmp(cid, id)) {
      if (found)
        die("duplicate choice: %s", id);
      found = n;
    }
    free(cid);
  }
  if (!found)
    die("unresolved choice: %s", id);
  return found;
}

typedef struct {
  char *path, *active, *version;
  int found;
} Reference;

/* Reference attributes are merged across global and choice-local nodes.
 * Conflicting definitions are ambiguous and refused rather than guessed. */
static void merge_attribute(char **out, xmlNode *n, const char *key) {
  if (!xmlHasProp(n, BAD_CAST key))
    return;
  char *value = xml_attr(n, key, NULL);
  if (*out && strcmp(*out, value))
    die("conflicting pkg-ref %s definitions", key);
  if (!*out)
    *out = value;
  else
    free(value);
}

static int sibling; /* references are sibling files (-dist) */

static void collect_reference(xmlNode *n, const char *id, Reference *ref) {
  for (; n; n = xml_next_element(n)) {
    if (xml_is(n, "pkg-ref")) {
      char *nid = xml_attr(n, "id", NULL);
      if (!strcmp(nid, id)) {
        ref->found = 1;
        merge_attribute(&ref->active, n, "active");
        merge_attribute(&ref->version, n, "version");
        for (xmlNode *t = n->children; t; t = t->next)
          if (t->type == XML_TEXT_NODE || t->type == XML_CDATA_SECTION_NODE) {
            char *text = xml_text(t);
            char *start = text;
            while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
              start++;
            size_t len = strlen(start);
            while (len && (start[len - 1] == ' ' || start[len - 1] == '\t' ||
                           start[len - 1] == '\r' || start[len - 1] == '\n'))
              start[--len] = 0;
            if (*start) {
              if (ref->path)
                die("pkg-ref %s has several locations", id);
              ref->path = sibling ? sibling_reference(start)
                                  : embedded_reference(start);
            }
            free(text);
          }
      }
      free(nid);
    }
    collect_reference(xml_elements(n), id, ref);
  }
}

static Reference find_reference(xmlNode *dist, const char *id) {
  Reference ref = {0};
  collect_reference(xml_elements(dist), id, &ref);
  if (!ref.found || !ref.path)
    die("unresolved pkg-ref: %s", id);
  return ref;
}

static void free_reference(Reference *ref) {
  free(ref->path);
  free(ref->active);
  free(ref->version);
}

typedef struct {
  char *id;
  xmlNode *node;
  JSValue value;
} Choice;

typedef struct {
  Choice *items;
  size_t count;
} ChoiceList;

/* Front ends other than Installer have their own outlines, named by `ui`
 * (Software Update's is ui="SoftwareUpdate"). Installer's is the one
 * without, and that is the one mdpkg follows. */
static xmlNode *main_outline(xmlNode *dist) {
  xmlNode *found = NULL;
  for (xmlNode *n = xml_elements(dist); n; n = xml_next_element(n)) {
    if (!xml_is(n, "choices-outline"))
      continue;
    char *ui = xml_attr(n, "ui", "");
    for (xmlNode *m = xml_elements(dist); m != n; m = xml_next_element(m))
      if (xml_is(m, "choices-outline")) {
        char *other = xml_attr(m, "ui", "");
        if (!strcmp(ui, other))
          die("duplicate Distribution element: choices-outline%s%s",
              *ui ? " ui=" : "", ui);
        free(other);
      }
    if (!*ui)
      found = n;
    free(ui);
  }
  if (!found)
    die("Distribution without a choices-outline");
  return found;
}

static void outline_choices(xmlNode *dist, xmlNode *line, ChoiceList *choices,
                              int depth) {
  if (depth > LIMIT_DEPTH)
    die("choices-outline nesting limit");
  for (; line; line = xml_next_element(line)) {
    if (!xml_is(line, "line"))
      die("unsupported choices-outline element: %s", xml_name(line));
    char *id = xml_attr(line, "choice", NULL);
    for (size_t i = 0; i < choices->count; i++)
      if (!strcmp(choices->items[i].id, id))
        die("choice appears more than once in outline: %s", id);
    if (choices->count >= 1024)
      die("choice limit");
    xmlNode *node = find_choice(dist, id);
    choices->items = xrealloc(choices->items, choices->count + 1, sizeof(Choice));
    choices->items[choices->count++] = (Choice){id, node, JS_UNDEFINED};
    outline_choices(dist, xml_elements(line), choices, depth + 1);
  }
}

static void check_choices(xmlNode *dist, ChoiceList *choices) {
  for (xmlNode *n = xml_elements(dist); n; n = xml_next_element(n)) {
    if (xml_is(n, "choice")) {
      char *id = xml_attr(n, "id", NULL);
      (void)find_choice(dist, id);
      int found = 0;
      for (size_t i = 0; i < choices->count; i++)
        if (!strcmp(id, choices->items[i].id))
          found = 1;
      if (!found)
        die("choice missing from outline: %s", id);
      free(id);
      for (xmlNode *child = xml_elements(n); child; child = xml_next_element(child)) {
        if (!xml_is(child, "pkg-ref"))
          die("unsupported choice content: %s", xml_name(child));
        char *pid = xml_attr(child, "id", NULL);
        Reference ref = find_reference(dist, pid);
        free_reference(&ref);
        free(pid);
      }
    }
  }
}

static void set_property(InstallerJS *js, JSValueConst object, const char *key,
                          JSValue value) {
  installer_js_require(js, value, key);
  if (JS_SetPropertyStr(js->context, object, key, value) < 0)
    installer_js_require(js, JS_EXCEPTION, key);
}

static int choice_state(InstallerJS *js, Choice *choice, const char *key) {
  JSValue value = JS_GetPropertyStr(js->context, choice->value, key);
  installer_js_require(js, value, choice->id);
  int state = JS_ToBool(js->context, value);
  JS_FreeValue(js->context, value);
  return state;
}

static void initialize_choices(InstallerJS *js, xmlNode *dist, ChoiceList *choices) {
  const char *keys[] = {"selected", "enabled", "visible"};
  const char *initial[] = {"start_selected", "start_enabled", "start_visible"};
  for (size_t i = 0; i < choices->count; i++) {
    Choice *choice = &choices->items[i];
    choice->value = JS_NewObject(js->context);
    installer_js_require(js, choice->value, choice->id);
    for (size_t k = 0; k < 3; k++)
      set_property(js, choice->value, keys[k], JS_NewBool(js->context,
          static_boolean(choice->node, initial[k])));
    const char *metadata[] = {"title", "description"};
    for (size_t k = 0; k < 2; k++) {
      char *text = xml_attr(choice->node, metadata[k], "");
      set_property(js, choice->value, metadata[k], JS_NewString(js->context, text));
      free(text);
    }
    JSValue packages = JS_NewArray(js->context);
    uint32_t index = 0;
    for (xmlNode *n = xml_elements(choice->node); n; n = xml_next_element(n)) {
      char *id = xml_attr(n, "id", NULL);
      Reference ref = find_reference(dist, id);
      JSValue package = JS_NewObject(js->context);
      set_property(js, package, "identifier", JS_NewString(js->context, id));
      set_property(js, package, "version", ref.version ? JS_NewString(js->context, ref.version) : JS_UNDEFINED);
      if (JS_SetPropertyUint32(js->context, packages, index++, package) < 0)
        installer_js_require(js, JS_EXCEPTION, choice->id);
      free_reference(&ref);
      free(id);
    }
    set_property(js, choice->value, "packages", packages);
    /* Expose an explicit failure for the intentionally unsupported property. */
    JSValue source = installer_js_eval(js,
        "({get packageUpgradeAction(){throw new TypeError('unsupported Installer JS API: choice.packageUpgradeAction')}})",
        "choice API", 1);
    JSPropertyDescriptor descriptor;
    JSAtom atom = JS_NewAtom(js->context, "packageUpgradeAction");
    if (JS_GetOwnProperty(js->context, &descriptor, source, atom) != 1)
      installer_js_require(js, JS_EXCEPTION, choice->id);
    if (JS_DefinePropertyGetSet(js->context, choice->value, atom,
                                descriptor.getter, descriptor.setter, JS_PROP_ENUMERABLE) < 0)
      installer_js_require(js, JS_EXCEPTION, choice->id);
    JS_FreeValue(js->context, descriptor.value);
    JS_FreeAtom(js->context, atom);
    JS_FreeValue(js->context, source);
    set_property(js, js->choices, choice->id, JS_DupValue(js->context, choice->value));
  }
}

static void settle_choices(InstallerJS *js, ChoiceList *choices) {
  const char *keys[] = {"selected", "enabled", "visible"};
  int *before = xcalloc(choices->count * 3, sizeof(int));
  for (int pass = 0; pass < 64; pass++) {
    for (size_t i = 0; i < choices->count; i++)
      for (size_t k = 0; k < 3; k++)
        before[i * 3 + k] = choice_state(js, &choices->items[i], keys[k]);
    for (size_t i = 0; i < choices->count; i++) {
      Choice *choice = &choices->items[i];
      installer_js_my(js, choice->value);
      for (size_t k = 0; k < 3; k++) {
        if (!xmlHasProp(choice->node, BAD_CAST keys[k]))
          continue;
        char *source = xml_attr(choice->node, keys[k], NULL), label[512];
        expression_label(label, sizeof(label), choice->node, keys[k]);
        int value = installer_js_boolean(js, source, label);
        free(source);
        set_property(js, choice->value, keys[k], JS_NewBool(js->context, value));
      }
    }
    int changed = 0;
    for (size_t i = 0; i < choices->count; i++)
      for (size_t k = 0; k < 3; k++)
        if (before[i * 3 + k] != choice_state(js, &choices->items[i], keys[k]))
          changed = 1;
    if (!changed) {
      free(before);
      return;
    }
  }
  die("Installer JS choices did not stabilize after 64 passes");
}

static void check_payload_paths(Package *pkg) {
  if (!pkg->count)
    die("no package selected");
  EntryList all = {0};
  for (size_t i = 0; i < pkg->count; i++)
    for (size_t j = 0; j < pkg->components[i].payload.count; j++)
      entries_append(&all, pkg->components[i].payload.items[j]);
  entries_check_paths(&all, 1);
  free(all.items);
}

void package_resolve(Package *pkg, const char *root, int live) {
  if (!pkg->distribution)
    return;
  if (pkg->needs_js && !root)
    die("Installer JS selection requires a target");
  xmlNode *dist = xmlDocGetRootElement(pkg->distribution);
  ChoiceList choices = {0};
  outline_choices(dist, xml_elements(main_outline(dist)), &choices, 0);
  InstallerJS *js = pkg->needs_js ? installer_js_new(root, live) : NULL;
  if (js) {
    js->strings = pkg->strings;
    initialize_choices(js, dist, &choices);
    xmlNode *script = xml_child(dist, "script");
    if (script) {
      char *source = xml_text(script);
      JS_FreeValue(js->context, installer_js_eval(js, source, "Distribution/script", 0));
      free(source);
    }
    installer_js_check(js, xml_child(dist, "installation-check"));
    installer_js_check(js, xml_child(dist, "volume-check"));
    settle_choices(js, &choices);
  }
  for (size_t i = 0; i < choices.count; i++) {
    Choice *choice = &choices.items[i];
    int selected = js ? choice_state(js, choice, "selected")
                      : static_boolean(choice->node, "start_selected") &&
                        static_boolean(choice->node, "selected");
    if (selected)
      for (xmlNode *n = xml_elements(choice->node); n; n = xml_next_element(n)) {
        char *id = xml_attr(n, "id", NULL);
        Reference ref = find_reference(dist, id);
        int active = 1;
        if (ref.active) {
          if (js) {
            installer_js_my(js, choice->value);
            char label[512];
            snprintf(label, sizeof(label), "pkg-ref[%s]/@active", id);
            active = installer_js_boolean(js, ref.active, label);
          } else
            active = !strcmp(ref.active, "true");
        }
        if (active)
          load_component(pkg, ref.path, id, 0);
        free_reference(&ref);
        free(id);
      }
    if (js)
      JS_FreeValue(js->context, choice->value);
    free(choice->id);
  }
  free(choices.items);
  if (js)
    installer_js_free(js);
  xml_free(pkg->distribution);
  pkg->distribution = NULL;
  check_payload_paths(pkg);
}

void package_inspect_candidates(Package *pkg) {
  xmlNode *dist = xmlDocGetRootElement(pkg->distribution);
  for (xmlNode *choice = xml_elements(dist); choice; choice = xml_next_element(choice)) {
    if (!xml_is(choice, "choice"))
      continue;
    for (xmlNode *n = xml_elements(choice); n; n = xml_next_element(n)) {
      char *id = xml_attr(n, "id", NULL);
      Reference ref = find_reference(dist, id);
      load_component(pkg, ref.path, id, 1);
      free_reference(&ref);
      free(id);
    }
  }
  pkg->unresolved = 1;
}

/* One quoted .strings token: "..." with C escapes, or '...' taken as is. */
static char *strings_token(const char **p) {
  char quote = **p;
  if (quote != '"' && quote != '\'')
    die("localization strings: expected a quoted string");
  size_t cap = strlen(*p), n = 0;
  char *out = xcalloc(cap + 1, 1);
  for ((*p)++; **p != quote; (*p)++) {
    if (!**p)
      die("localization strings: unterminated string");
    char ch = **p;
    if (quote == '"' && ch == '\\') {
      ch = *++*p;
      ch = ch == 'n' ? '\n' : ch == 't' ? '\t' : ch == 'r' ? '\r'
           : ch == '"' || ch == '\\' || ch == '\'' ? ch : 0;
      if (!ch)
        die("localization strings: unsupported escape");
    }
    out[n++] = ch;
  }
  (*p)++;
  return out;
}

static void skip_strings_space(const char **p) {
  for (;;) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
      (*p)++;
    if ((*p)[0] == '/' && (*p)[1] == '*') {
      const char *end = strstr(*p + 2, "*/");
      if (!end)
        die("localization strings: unterminated comment");
      *p = end + 2;
    } else if ((*p)[0] == '/' && (*p)[1] == '/')
      *p += strcspn(*p, "\n");
    else
      return;
  }
}

/* The English table of the Distribution's <localization>, read by
 * system.localizedString. Other languages are not used. */
static void read_localization(Package *pkg, xmlNode *dist) {
  xmlNode *localization = xml_child(dist, "localization");
  size_t count = 0;
  int seen = 0;
  pkg->strings = xcalloc(1, sizeof(char *));
  for (xmlNode *n = xml_elements(localization); n; n = xml_next_element(n)) {
    char *language = xml_attr(n, "language", "");
    int english = !strcmp(language, "English") || !strcmp(language, "en");
    free(language);
    if (!english)
      continue;
    if (seen++)
      die("more than one English localization table");
    char *text = xml_text(n);
    const char *p = text;
    for (skip_strings_space(&p); *p; skip_strings_space(&p)) {
      if (count >= 2048)
        die("localization strings limit");
      char *key = strings_token(&p);
      skip_strings_space(&p);
      if (*p++ != '=')
        die("localization strings: expected '=' after \"%s\"", key);
      skip_strings_space(&p);
      char *value = strings_token(&p);
      skip_strings_space(&p);
      if (*p++ != ';')
        die("localization strings: expected ';' after \"%s\"", key);
      for (size_t i = 0; i < count; i += 2)
        if (!strcmp(pkg->strings[i], key))
          die("duplicate localization string: %s", key);
      pkg->strings = xrealloc(pkg->strings, count + 3, sizeof(char *));
      pkg->strings[count++] = key;
      pkg->strings[count++] = value;
      pkg->strings[count] = NULL;
    }
    free(text);
  }
}

static void distribution_load(Package *pkg, Bytes distribution) {
  pkg->distribution = xml_parse(distribution);
  xmlNode *dist = xmlDocGetRootElement(pkg->distribution);
  if (!xml_is(dist, "installer-script") && !xml_is(dist, "installer-gui-script"))
    die("unsupported Distribution root");
  (void)main_outline(dist);
  const char *unique[] = {"script", "installation-check", "volume-check", "options", "localization"};
  for (size_t i = 0; i < sizeof(unique) / sizeof(*unique); i++) {
    int seen = 0;
    for (xmlNode *n = xml_elements(dist); n; n = xml_next_element(n))
      if (xml_is(n, unique[i]) && seen++)
        die("duplicate Distribution element: %s", unique[i]);
  }
  xmlNode *options = xml_child(dist, "options");
  int require_scripts = static_boolean(options, "require-scripts");
  InstallerJS *js = installer_js_new(NULL, 0);
  validate_distribution(pkg, js, dist, 0, require_scripts);
  installer_js_free(js);
  ChoiceList choices = {0};
  outline_choices(dist, xml_elements(main_outline(dist)), &choices, 0);
  check_choices(dist, &choices);
  for (size_t i = 0; i < choices.count; i++)
    free(choices.items[i].id);
  free(choices.items);
  read_localization(pkg, dist);
}

void package_load(Package *pkg, const char *path) {
  *pkg = (Package){.path = path};
  xar_open(&pkg->xar, path);
  Bytes distribution = xar_member(&pkg->xar, "Distribution", 0);
  if (!distribution.data) {
    load_component(pkg, "", NULL, 0);
    check_payload_paths(pkg);
    return;
  }
  distribution_load(pkg, distribution);
}

void package_load_distribution(Package *pkg, const char *path,
                               const char *pkg_dir) {
  *pkg = (Package){.path = path, .pkg_dir = pkg_dir};
  sibling = 1;
  Bytes distribution = file_read(path, LIMIT_XML);
  distribution_load(pkg, distribution);
  free(distribution.data);
}
