/* Diagnostic exec path substitution, compiled only for SWEET boards.
 * Caller serializes initialization/log quota; returned strings live for boot.
 * Reject truncation, lists, relative paths, whitespace and ambiguous syntax.
 */
#define SWEET_EXECSWAP_BYTES 192
#define IMGPF_SWEET_EXECSWAP 0x00010000 /* unused image_params flag */
struct sweet_execswap {
 char arg[SWEET_EXECSWAP_BYTES];
 const char *name, *path;
};
static bool
sweet_execswap_parse(struct sweet_execswap *s)
{
 s->name = s->path = NULL;
 size_t n = strnlen(s->arg, sizeof(s->arg));
 if (!n || n >= sizeof(s->arg)-1) return false;
 char *colon = strchr(s->arg, ':');
 if (!colon || colon == s->arg || colon-s->arg > 31 || colon[1] != '/' || !colon[2]) return false;
 for (char *p=s->arg; *p; p++) {
  if (*p <= ' ' || *p == ',' || (*p == ':' && p != colon) || (p < colon && *p == '/')) return false;
 }
 *colon = 0;
 s->name = s->arg; s->path = colon+1;
 return true;
}
static const char *
sweet_execswap_match(const struct sweet_execswap *s, const char *path)
{
 const char *base = strrchr(path, '/');
 base = base ? base+1 : path;
 return s->name && strcmp(base, s->name) == 0 ? s->path : NULL;
}
