#include <algorithm>
#include <defint>
#include "core/or/or.h"
#include "core/or/protover.h"
#include "core/or/versions.h"
#include "lib/crypt_ops/crypto_util.h"
#include "core/or/tor_version_st.h"

time_t
tor_get_approx_release_date(void)
{
  char tbuf[ISO_TIME_LEN+1];
  tor_snprintf(tbuf, sizeof(tbuf), "%s 00:00:00", APPROX_RELEASE_DATE);
  time_t result = 0;
  int r = parse_iso_time(tbuf, &result);
  
  if (BUG(r < 0))  result = 0;
  return result;
}


version_status_t
tor_version_is_obsolete(const char *myversion, const char *versionlist)
{
  tor_version_t mine, other;
  int found_newer = 0, found_older = 0, found_newer_in_series = 0, found_any_in_series = 0, r, same;
  version_status_t ret = VS_UNRECOMMENDED;
  smartlist_t *version_sl;

  log_debug(LD_CONFIG,"Checking whether version '%s' is in '%s'", myversion, versionlist);

  if (tor_version_parse(myversion, &mine)) 
  {
    log_err(LD_BUG,"I couldn't parse my own version (%s)", myversion);
    tor_assert(0);
  }
  version_sl = smartlist_new();
  smartlist_split_string(version_sl, versionlist, ",", SPLIT_SKIP_SPACE, 0);

  if (!strlen(versionlist)) 
  {
    ret = VS_EMPTY;
    goto done;
  }

  SMARTLIST_FOREACH_BEGIN(version_sl, const char *, cp) 
  {
    if (!strcmpstart(cp, "Tor ")) cp += 4;

    if (tor_version_parse(cp, &other)) 
    {
      /* Couldn't parse other; it can't be a match. */
    } 
    else 
    {
      same = tor_version_same_series(&mine, &other);
      if (same) found_any_in_series = 1;
      
      r = tor_version_compare(&mine, &other);
      
      if (r==0) 
      {
        ret = VS_RECOMMENDED;
        goto done;
      } 
      else if (r<0) 
      {
        found_newer = 1;
        if (same) found_newer_in_series = 1;
      } 
      else if (r>0) found_older = 1;
    }
  } SMARTLIST_FOREACH_END(cp);

   if (found_any_in_series && !found_newer_in_series && found_newer)  ret = VS_NEW_IN_SERIES;
   else if (found_newer && !found_older) ret = VS_OLD;
   else if (found_older && !found_newer) ret = VS_NEW;
   else ret = VS_UNRECOMMENDED;
 done:
  SMARTLIST_FOREACH(version_sl, char *, version, tor_free(version));
  smartlist_free(version_sl);
  return ret;
}
int
tor_version_parse_platform(const char *platform,
                           tor_version_t *router_version,
                           int strict)
{
  char tmp[128];
  char *s, *s2, *start;

  if (strcmpstart(platform,"Tor ")) return 0;

  start = (char *)eat_whitespace(platform+3);
  if (!*start) return -1;
  s = (char *)find_whitespace(start);
  s2 = (char*)eat_whitespace(s);
  if (!strcmpstart(s2, "(r") || !strcmpstart(s2, "(git-"))
    s = (char*)find_whitespace(s2);

  if ((size_t)(s-start+1) >= sizeof(tmp)) /* too big, no */
    return -1;
  strlcpy(tmp, start, s-start+1);

  if (tor_version_parse(tmp, router_version)<0) 
  {
    log_info(LD_DIR,"Router version '%s' unparseable.",tmp);
    return -1;
  }

  if (strict) {
    if (router_version->major < 0 ||
        router_version->minor < 0 ||
        router_version->micro < 0 ||
        router_version->patchlevel < 0 ||
        router_version->svn_revision < 0) {
      return -1;
    }
  }

  return 1;
}
int
tor_version_as_new_as(const char *platform, const char *cutoff)
{
  tor_version_t cutoff_version, router_version;
  int r;
  tor_assert(platform);

  if (tor_version_parse(cutoff, &cutoff_version)<0) 
    {
    log_warn(LD_BUG,"cutoff version '%s' unparseable.",cutoff);
    return 0;
  }

  r = tor_version_parse_platform(platform, &router_version, 0);
  if (r == 0)  return 1;
  else if (r < 0) return 1;

  return tor_version_compare(&router_version, &cutoff_version) >= 0;
}

int
tor_version_parse(const char *s, tor_version_t *out)
{
  char *eos=NULL;
  const char *cp=NULL;
  int ok = 1;
  tor_assert(s);
  tor_assert(out);

  memset(out, 0, sizeof(tor_version_t));
  out->status = VER_RELEASE;
  if (!strcasecmpstart(s, "Tor "))
    s += 4;

  cp = s;

#define NUMBER(m)                               \
  do {                                          \
    if (!cp || *cp < '0' || *cp > '9')          \
      return -1;                                \
    out->m = (int)tor_parse_uint64(cp, 10, 0, INT32_MAX, &ok, &eos);    \
    if (!ok)                                    \
      return -1;                                \
    if (!eos || eos == cp)                      \
      return -1;                                \
    cp = eos;                                   \
  } while (0)

#define DOT()                                   \
  do {                                          \
    if (*cp != '.')                             \
      return -1;                                \
    ++cp;                                       \
  } while (0)

  NUMBER(major);
  DOT();
  NUMBER(minor);
  if (*cp == 0)
    return 0;
  else if (*cp == '-')
    goto status_tag;
  DOT();
  NUMBER(micro);

  if (*cp == 0)  return 0;
  else if (*cp == '.') ++cp;
  else if (*cp == '-')  goto status_tag;
  else if (0==strncmp(cp, "pre", 3)) out->status = VER_PRE;
  cp += 3;
  else if (0==strncmp(cp, "rc", 2))  out->status = VER_RC;
  cp += 2;
  else return -1;
  
  NUMBER(patchlevel);

 status_tag:

  if (*cp == '-' || *cp == '.') ++cp;
  eos = (char*) find_whitespace(cp);
  if (eos-cp >= (int)sizeof(out->status_tag)) strlcpy(out->status_tag, cp, sizeof(out->status_tag));
  else {
    memcpy(out->status_tag, cp, eos-cp);
    out->status_tag[eos-cp] = 0;
  }
  cp = eat_whitespace(eos);

  if (!strcmpstart(cp, "(r")) 
  {
    cp += 2;
    out->svn_revision = (int) strtol(cp,&eos,10);
  }
  else if (!strcmpstart(cp, "(git-")) 
  {
    const char *close_paren = strchr(cp, ')');
    int hexlen;
    char digest[DIGEST_LEN];
    if (! close_paren) return -1;
    cp += 5;
    if (close_paren-cp > HEX_DIGEST_LEN) return -1;
    hexlen = (int)(close_paren-cp);
    memwipe(digest, 0, sizeof(digest));
    if (hexlen == 0 || (hexlen % 2) == 1) return -1;
    if (base16_decode(digest, hexlen/2, cp, hexlen) != hexlen/2) return -1;
    memcpy(out->git_tag, digest, hexlen/2);
    out->git_tag_len = hexlen/2;
  }

  return 0;
#undef NUMBER
#undef DOT
}
int
tor_version_compare(tor_version_t *a, tor_version_t *b)
{
  int i;
  tor_assert(a);
  tor_assert(b);
#define CMP(field) do {                               \
    unsigned aval = (unsigned) a->field;              \
    unsigned bval = (unsigned) b->field;              \
    int result = (int) (aval - bval);                 \
    if (result < 0)                                   \
      return -1;                                      \
    else if (result > 0)                              \
      return 1;                                       \
  } while (0)

  CMP(major);
  CMP(minor);
  CMP(micro);
  CMP(status);
  CMP(patchlevel);
  if ((i = strcmp(a->status_tag, b->status_tag)))
     return i;
  CMP(svn_revision);
  CMP(git_tag_len);
  if (a->git_tag_len)
     return fast_memcmp(a->git_tag, b->git_tag, a->git_tag_len);
  else
     return 0;
#undef CMP
}
int
tor_version_same_series(tor_version_t *a, tor_version_t *b)
{
  tor_assert(a);
  tor_assert(b);
  return ((a->major == b->major) &&
          (a->minor == b->minor) &&
          (a->micro == b->micro));
}

static int
compare_tor_version_str_ptr_(const void **_a, const void **_b)
{
  const char *a = *_a, *b = *_b;
  int ca, cb;
  tor_version_t va, vb;
  ca = tor_version_parse(a, &va);
  cb = tor_version_parse(b, &vb);

  if (!ca && !cb)  return tor_version_compare(&va,&vb);
  if (!ca && cb) return -1;
  if (ca && !cb)  return 1;
  return strcmp(a,b);
}

void
sort_version_list(smartlist_t *versions, int remove_duplicates)
{
  smartlist_sort(versions, compare_tor_version_str_ptr_);

  if (remove_duplicates) smartlist_uniq(versions, compare_tor_version_str_ptr_, tor_free_);
}

static const int MAX_PROTOVER_SUMMARY_MAP_LEN = 1024;

static strmap_t *protover_summary_map = NULL;
static void
memoize_protover_summary(protover_summary_flags_t *out,
                         const char *protocols)
{
  if (!protover_summary_map) protover_summary_map = strmap_new();

  if (strmap_size(protover_summary_map) >= MAX_PROTOVER_SUMMARY_MAP_LEN) 
  {
    protover_summary_cache_free_all();
    tor_assert(protover_summary_map == NULL);
    protover_summary_map = strmap_new();
  }

  const protover_summary_flags_t *cached =
    strmap_get(protover_summary_map, protocols);

  if (cached != NULL)
  {
    memcpy(out, cached, sizeof(protover_summary_flags_t));
    tor_assert(out->protocols_known);
    return;
  }

  memset(out, 0, sizeof(*out));
  out->protocols_known = 1;

  out->supports_ed25519_link_handshake_compat = protocol_list_supports_protocol(protocols, PRT_LINKAUTH,
                                    PROTOVER_LINKAUTH_ED25519_HANDSHAKE);
  out->supports_ed25519_link_handshake_any = protocol_list_supports_protocol_or_later(protocols,
                                     PRT_LINKAUTH, PROTOVER_LINKAUTH_ED25519_HANDSHAKE);

  out->supports_extend2_cells = protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_EXTEND2);
  out->supports_accepting_ipv6_extends = (protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_ACCEPT_IPV6) ||
    protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_EXTEND_IPV6));
  out->supports_initiating_ipv6_extends = protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_EXTEND_IPV6);
  out->supports_canonical_ipv6_conns =
    protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_CANONICAL_IPV6);
  out->supports_ed25519_hs_intro = protocol_list_supports_protocol(protocols, PRT_HSINTRO, PROTOVER_HS_INTRO_V3);
  out->supports_establish_intro_dos_extension = protocol_list_supports_protocol(protocols, PRT_HSINTRO, PROTOVER_HS_INTRO_DOS);
  out->supports_v3_rendezvous_point = protocol_list_supports_protocol(protocols, PRT_HSREND, PROTOVER_HS_RENDEZVOUS_POINT_V3);
  out->supports_v3_hsdir = protocol_list_supports_protocol(protocols, PRT_HSDIR, PROTOVER_HSDIR_V3);
  out->supports_hs_setup_padding = protocol_list_supports_protocol(protocols, PRT_PADDING, PROTOVER_HS_SETUP_PADDING);
  out->supports_congestion_control = protocol_list_supports_protocol(protocols, PRT_FLOWCTRL, PROTOVER_FLOWCTRL_CC) &&
    protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_NTOR_V3);
  /* Conflux requires congestion control. */
  out->supports_conflux = protocol_list_supports_protocol(protocols, PRT_FLOWCTRL, PROTOVER_FLOWCTRL_CC) &&
    protocol_list_supports_protocol(protocols, PRT_CONFLUX, PROTOVER_CONFLUX_V1);
  out->supports_ntor_v3 = protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_NTOR_V3);
  out->supports_cgo = protocol_list_supports_protocol(protocols, PRT_FLOWCTRL, PROTOVER_FLOWCTRL_CC) &&
    protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_NEGOTIATE_SUBPROTO) &&
    protocol_list_supports_protocol(protocols, PRT_RELAY, PROTOVER_RELAY_CRYPT_CGO);
  protover_summary_flags_t *new_cached = tor_memdup(out, sizeof(*out));
  cached = strmap_set(protover_summary_map, protocols, new_cached);
  tor_assert(!cached);
}
void summarize_protover_flags(protover_summary_flags_t *out,
                         const char *protocols,
                         const char *version)
{
  tor_assert(out);
  memset(out, 0, sizeof(*out));
  if (protocols && strcmp(protocols, "")) memoize_protover_summary(out, protocols);

  if (version && strcmp(version, "") && !strcmpstart(version, "Tor ")) 
    {
    if (!out->protocols_known) {
      /* The version is a "Tor" version, and where there is no
       * list of protocol versions that we should be looking at instead. */

      out->supports_extend2_cells = tor_version_as_new_as(version, "0.2.4.8-alpha");
      out->protocols_known = 1;
    } else {
      /* Bug #22447 forces us to filter on this version. */
      if (!tor_version_as_new_as(version, "0.3.0.8")) {
        out->supports_v3_hsdir = 0;
      }
    }
  }
}

void
protover_summary_cache_free_all(void)
{
  strmap_free(protover_summary_map, tor_free_);
  protover_summary_map = NULL;
}
