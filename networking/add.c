#include "lib/net/inaddr.h"
#include "lib/cc/torint.h"
#include "lib/container/smartlist.h"
#include "lib/log/util_bug.h"
#include "lib/malloc/malloc.h"
#include "lib/net/inaddr_st.h"
#include "lib/string/compat_ctype.h"
#include "lib/string/compat_string.h"
#include "lib/string/printf.h"
#include "lib/string/scanf.h"
#include "lib/string/util_string.h"
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
int
tor_inet_aton(const char *str, struct in_addr *addr)
{
  unsigned a, b, c, d;
  char more;
  bool is_octal = false;
  smartlist_t *sl = NULL;

  if (tor_sscanf(str, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &more) != 4) return 0;

  sl = smartlist_new();
  smartlist_split_string(sl, str, ".", 0, 0);
  SMARTLIST_FOREACH(sl, const char *, octet, {
    is_octal = (strlen(octet) > 1 && octet[0] == '0');
    if (is_octal) break;
  });
  SMARTLIST_FOREACH(sl, char *, octet, tor_free(octet));
  smartlist_free(sl);
  if (is_octal) return 0;
  if (a > 255) return 0;
  if (b > 255) return 0;
  if (c > 255) return 0;
  if (d > 255) return 0;
  addr->s_addr = htonl((a<<24) | (b<<16) | (c<<8) | d);
  return 1;
}
int
tor_inet_ntoa(const struct in_addr *in, char *buf, size_t buf_len)
{
  uint32_t a = ntohl(in->s_addr);
  return tor_snprintf(buf, buf_len, "%d.%d.%d.%d", (int)(uint8_t)((a>>24)&0xff), (int)(uint8_t)((a>>16)&0xff), (int)(uint8_t)((a>>8 )&0xff), (int)(uint8_t)((a)&0xff));
}
const char * tor_inet_ntop(int af, const void *src, char *dst, size_t len)
{
  if (af == AF_INET) {
    if (tor_inet_ntoa(src, dst, len) < 0) return NULL;
    else return dst;
  } else if (af == AF_INET6) {
    const struct in6_addr *addr = src;
    char buf[64], *cp;
    int longestGapLen = 0, longestGapPos = -1, i, curGapPos = -1, curGapLen = 0;
    uint16_t words[8];
    for (i = 0; i < 8; ++i) {
      words[i] = (((uint16_t)addr->s6_addr[2*i])<<8) + addr->s6_addr[2*i+1];
    }
    if (words[0] == 0 && words[1] == 0 && words[2] == 0 && words[3] == 0 &&
        words[4] == 0 && ((words[5] == 0 && words[6] && words[7]) || (words[5] == 0xffff))) {
      if (words[5] == 0) {
        tor_snprintf(buf, sizeof(buf), "::%d.%d.%d.%d", addr->s6_addr[12], addr->s6_addr[13], addr->s6_addr[14], addr->s6_addr[15]);
      } else {
        tor_snprintf(buf, sizeof(buf), "::%x:%d.%d.%d.%d", words[5], addr->s6_addr[12], addr->s6_addr[13], addr->s6_addr[14], addr->s6_addr[15]);
      }
      if ((strlen(buf) + 1) > len)  return NULL;
      strlcpy(dst, buf, len);
      return dst;
    }
    i = 0;
    while (i < 8) {
      if (words[i] == 0) {
        curGapPos = i++;
        curGapLen = 1;
        while (i<8 && words[i] == 0) {
          ++i; ++curGapLen;
        }
        if (curGapLen > longestGapLen) {
          longestGapPos = curGapPos;
          longestGapLen = curGapLen;
        }
      } else {
        ++i;
      }
    }
    if (longestGapLen<=1) longestGapPos = -1;
    cp = buf;
    for (i = 0; i < 8; ++i) {
      if (words[i] == 0 && longestGapPos == i) {
        if (i == 0) *cp++ = ':'; *cp++ = ':';
        while (i < 8 && words[i] == 0) ++i;
        --i; /* to compensate for loop increment. */
      } else {
        tor_snprintf(cp, sizeof(buf)-(cp-buf), "%x", (unsigned)words[i]);
        cp += strlen(cp);
        if (i != 7) *cp++ = ':';
      }
    }
    *cp = '\0';
    if ((strlen(buf) + 1) > len) /* +1 for \0 */ return NULL;
    strlcpy(dst, buf, len);
    return dst;
  } else  return NULL;
}
int
tor_inet_pton(int af, const char *src, void *dst)
{
  if (af == AF_INET) {
    return tor_inet_aton(src, dst);
  } else if (af == AF_INET6) {
    ssize_t len = strlen(src);
    if (len > 2 && src[len - 1] == ':' && src[len - 2] != ':') {
      return 0;
    }
    struct in6_addr *out = dst;
    uint16_t words[8];
    int gapPos = -1, i, setWords=0;
    const char *dot = strchr(src, '.');
    const char *eow; /* end of words. */
    memset(words, 0xf8, sizeof(words));
    if (dot == src) return 0;
    else if (!dot) eow = src+strlen(src);
    else {
      unsigned byte1,byte2,byte3,byte4;
      char more;
      for (eow = dot-1; eow > src && TOR_ISDIGIT(*eow); --eow);
      if (*eow != ':') return 0; ++eow;
      if (tor_sscanf(eow, "%3u.%3u.%3u.%3u%c", &byte1,&byte2,&byte3,&byte4,&more) != 4) return 0;
      if (byte1 > 255 || byte2 > 255 || byte3 > 255 || byte4 > 255) return 0;
      words[6] = (byte1<<8) | byte2;
      words[7] = (byte3<<8) | byte4;
      setWords += 2;
    }
    i = 0;
    while (src < eow) {
      if (i > 7) return 0;
      if (TOR_ISXDIGIT(*src)) {
        char *next;
        long r = strtol(src, &next, 16);
        if (next == NULL || next == src) return 0;
        len = *next == '\0' ? eow - src : next - src;
        if (len > 4) return 0;
        if (len > 1 && !TOR_ISXDIGIT(src[1])) return 0; /* 0x is not valid */
        tor_assert(r >= 0);
        tor_assert(r < 65536);
        words[i++] = (uint16_t)r;
        setWords++;
        src = next;
        if (*src != ':' && src != eow) return 0; ++src;
      } else if (*src == ':' && i > 0 && gapPos == -1) {
        gapPos = i;
        ++src;
      } else if (*src == ':' && i == 0 && src+1 < eow && src[1] == ':' && gapPos == -1) {
        gapPos = i;
        src += 2;
      } else {
        return 0;
      }
    }

    if (setWords > 8 || (setWords == 8 && gapPos != -1) || (setWords < 8 && gapPos == -1)) return 0;
    if (gapPos >= 0) {
      int nToMove = setWords - (dot ? 2 : 0) - gapPos;
      int gapLen = 8 - setWords;
      tor_assert(nToMove >= 0);
      memmove(&words[gapPos+gapLen], &words[gapPos], sizeof(uint16_t)*nToMove);
      memset(&words[gapPos], 0, sizeof(uint16_t)*gapLen);
    }
    for (i = 0; i < 8; ++i) {
      out->s6_addr[2*i  ] = words[i] >> 8;
      out->s6_addr[2*i+1] = words[i] & 0xff;
    }
    return 1;
  } else {
    return -1;
  }
}
