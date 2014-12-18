/* ------------------------------------------------------------------------- *
 * Copyright (C) 2012-2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: GPLv2
 * ------------------------------------------------------------------------- */

#include "evdev.h"

#include "mce-log.h"

#include <linux/input.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>

/** Number of elements in array of fixed size */
#define numof(a) (sizeof(a)/sizeof*(a))

/* Autogenerated lookup tables */
#include "evdev.inc"

/** Number to string lookup with bounds checking
 */
static const char *lookup(const char * const *lut, size_t cnt, int val)
{
  return ((size_t)val < cnt) ? lut[val] : 0;
}

/* Internal helper functions for looking up event codes by type */
static const char * syn_name(int ecode)
{
  return lookup(lut_syn, numof(lut_syn), ecode) ?: "SYN_???";
}

static const char * key_name(int ecode)
{
  return lookup(lut_key, numof(lut_key), ecode) ?: "KEY_???";
}

static const char * rel_name(int ecode)
{
  return lookup(lut_rel, numof(lut_rel), ecode) ?: "REL_???";
}

static const char * abs_name(int ecode)
{
  return lookup(lut_abs, numof(lut_abs), ecode) ?: "ABS_???";
}

static const char * msc_name(int ecode)
{
  return lookup(lut_msc, numof(lut_msc), ecode) ?: "MSC_???";
}

static const char * led_name(int ecode)
{
  return lookup(lut_led, numof(lut_led), ecode) ?: "LED_???";
}

static const char * rep_name(int ecode)
{
  return lookup(lut_rep, numof(lut_rep), ecode) ?: "REP_???";
}

static const char * snd_name(int ecode)
{
  return lookup(lut_snd, numof(lut_snd), ecode) ?: "SND_???";
}

static const char * sw_name(int ecode)
{
  return lookup(lut_sw, numof(lut_sw), ecode) ?: "SW_???";
}

static const char * ff_name(int ecode)
{
  return lookup(lut_ff, numof(lut_ff), ecode) ?: "FF_???";
}

static const char * pwr_name(int ecode)
{
  (void)ecode; // not used
  return "PWR_???";
  //return lookup(lut_pwr, numof(lut_pwr), ecode) ?: "PWR_???";
}

/** How many "unsigned long" elements bitmap needs to cover bc bits */
#define BMAP_SIZE(bc) (((bc)+LONG_BIT-1)/LONG_BIT)

/** Test a bit in array of unsigned longs
 *
 * @param bmap array of longs
 * @param bi   bit index
 *
 * @return 1 if bit is set, 0 if not
 */
static int bit_is_set(unsigned long *bmap, size_t bi)
{
  size_t        i = bi / LONG_BIT;
  unsigned long m = 1ul << (bi % LONG_BIT);
  return (bmap[i] & m) != 0;
}

/** Helper for getting terminal width
 *
 * @return guestimate of number of characters per line
 */
static int get_terminal_width(void)
{
  int res;
  struct winsize ws;
  const char *env;

  if( (env = getenv("COLUMNS")) )
  {
    if( (res = strtol(env, 0, 10)) > 10 )
    {
      return res;
    }
  }

  res = 80;

  if( ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 ||
      ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) != -1 ||
      ioctl(STDIN_FILENO,  TIOCGWINSZ, &ws) != -1 )
  {
    res = ws.ws_col;
  }
  return res;
}

/** Lookup human readable name for input event code
 *
 * @param etype input event type
 * @param ecode input event code
 *
 * @return Name of input event code as string
 */
const char *evdev_get_event_code_name(int etype, int ecode)
{
  typedef const char *(*lookup_fn)(int);

  static const lookup_fn lut[] =
  {
    [EV_SYN] = syn_name,
    [EV_KEY] = key_name,
    [EV_REL] = rel_name,
    [EV_ABS] = abs_name,
    [EV_MSC] = msc_name,
    [EV_SW]  = sw_name,
    [EV_LED] = led_name,
    [EV_SND] = snd_name,
    [EV_REP] = rep_name,
    [EV_FF]  = ff_name,
    [EV_PWR] = pwr_name,
  };

  if( (size_t)etype < numof(lut) && lut[etype] )
  {
    return lut[etype](ecode);
  }
  return "???_???";
}

/** Lookup human readable name for input event type
 *
 * @param etype input event type
 *
 * @return Name of input event type as string
 */
const char * evdev_get_event_type_name(int etype)
{
  return lookup(lut_ev, numof(lut_ev), etype) ?: "EV_???";
}

/** Open input device in read only mode
 *
 * @param path input device path to open
 *
 * @returns file descriptor on success, or
 *          -1 if the file can't be opened / it is not an input device
 */
int evdev_open_device(const char *path)
{
  int fd;

  if( (fd = open(path, O_RDONLY)) != -1 )
  {
    int vers = 0;
    if( ioctl(fd, EVIOCGVERSION, &vers) == -1)
    {
      // assume this is not an input device node then
      close(fd), fd = -1;
    }
  }

  return fd;
}

/** Write input device information to stdout
 *
 * @param fd file descriptor
 *
 * @return 0 on success, or -1 in case of errors
 */
int evdev_identify_device(int fd)
{
  int err = -1;
  int vers;
  int cols;
  unsigned long bmap_type[BMAP_SIZE(EV_CNT)];
  unsigned long bmap_code[BMAP_SIZE(KEY_CNT)];
  unsigned long bmap_stat[BMAP_SIZE(KEY_CNT)];
  char path[256];
  int n;

  if( fd < 0 )
  {
    goto cleanup;
  }

  snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  if( (n = readlink(path, path, sizeof path - 1)) <= 0 )
  {
    strcpy(path, "unknown");
  }
  else
  {
    path[n] = 0;
  }

  // is it evdev file descriptor?
  if( ioctl(fd, EVIOCGVERSION, &vers) == -1)
  {
    goto cleanup;
  }

  // device name
  {
    char name[256];;
    if( ioctl(fd, EVIOCGNAME(sizeof(name)), name) == -1 )
    {
      mce_log(LL_WARN, "%s: EVIOCGNAME: %m", path);
      strcpy(name, "Unknown");
    }
    printf("Name: \"%s\"\n", name);
  }

  // device id
  {
    unsigned short id[4];
    if( ioctl(fd, EVIOCGID, id) == -1 )
    {
      mce_log(LL_WARN, "%s: EVIOCGID: %m", path);
      memset(id, 0, sizeof id);
    }
    printf("ID: bus 0x%x, vendor, 0x%x, product 0x%x, version 0x%x\n",
           id[ID_BUS], id[ID_VENDOR], id[ID_PRODUCT], id[ID_VERSION]);
  }

  // get bitmask of supported event types
  memset(bmap_type, 0, sizeof(bmap_type));
  if( ioctl(fd, EVIOCGBIT(0, EV_CNT), bmap_type) == -1 )
  {
    mce_log(LL_WARN, "%s: EVIOCGBIT(0): %m", path );
    goto cleanup;
  }

  // guestimate how wide event code listing we can make
  cols = get_terminal_width() - 10;
  if( cols < 32 ) cols = 72;

  // list supported event types and codes
  for( int etype = 0; etype < EV_CNT; ++etype )
  {
    if( !bit_is_set(bmap_type, etype) )
    {
      continue;
    }

    printf("Type 0x%02x (%s)\n", etype, evdev_get_event_type_name(etype));

    if( etype == EV_SYN || etype == EV_REP )
    {
      // EVIOCGBIT(0, n) returns event types supported, not
      // what SYN_xxx codes are supported ... skip it
      continue;
    }

    memset(bmap_code, 0, sizeof(bmap_code));
    if( ioctl(fd, EVIOCGBIT(etype, KEY_CNT), bmap_code) == -1 )
    {
      mce_log(LL_WARN, "%s: EVIOCGBIT(%s): %m", path,
              evdev_get_event_type_name(etype));
      continue;
    }

    memset(bmap_stat, 0, sizeof(bmap_stat));
    switch( etype )
    {
    case EV_KEY: ioctl(fd, EVIOCGKEY(KEY_CNT), bmap_stat); break;
    case EV_LED: ioctl(fd, EVIOCGLED(LED_CNT), bmap_stat); break;
    case EV_SND: ioctl(fd, EVIOCGSND(SND_CNT), bmap_stat); break;
    case EV_SW:  ioctl(fd, EVIOCGSW(SW_CNT),   bmap_stat); break;
    default: break;
    }

    int len = 0;
    for( int ecode = 0; ecode < KEY_CNT; ++ecode )
    {
      if( bit_is_set(bmap_code, ecode) )
      {
        const char *tag = evdev_get_event_code_name(etype, ecode);
        int set = bit_is_set(bmap_stat, ecode);
        int add = strlen(tag) + 1 + set;
        char val[32] = "";

        if( etype == EV_ABS )
        {
          struct input_absinfo info;
          memset(&info, 0, sizeof info);

          if( ioctl(fd, EVIOCGABS(ecode), &info) == -1 )
          {
            mce_log(LL_ERR, "EVIOCGABS(%s): %m", tag);
          }
          else
          {
            snprintf(val, sizeof val, "=%d [%d,%d]", info.value, info.minimum, info.maximum);
            add += strlen(val);
          }
        }

        if( len == 0 ) printf("\t");
        else if( len+add > cols ) printf("\n\t"), len = 0;

        len += add, printf(" %s%s%s", tag, set ? "*" : "", val);

      }
    }
    if( len ) printf("\n");
  }

  err = 0;

cleanup:

  return err;
}

static int rlookup(const char * const *lut, size_t cnt, const char *name)
{
  int val = -1;
  for( size_t i = 0; i < cnt; ++i )
  {
    if( lut[i] && !strcmp(lut[i], name) )
    {
      val = (int)i;
      break;
    }
  }
  return val;
}

/* Internal helper functions for looking up event codes by type */
static int syn_code(const char *ename)
{
  return rlookup(lut_syn, numof(lut_syn), ename);
}

static int key_code(const char *ename)
{
  return rlookup(lut_key, numof(lut_key), ename);
}

static int rel_code(const char *ename)
{
  return rlookup(lut_rel, numof(lut_rel), ename);
}

static int abs_code(const char *ename)
{
  return rlookup(lut_abs, numof(lut_abs), ename);
}

static int msc_code(const char *ename)
{
  return rlookup(lut_msc, numof(lut_msc), ename);
}

static int led_code(const char *ename)
{
  return rlookup(lut_led, numof(lut_led), ename);
}

static int rep_code(const char *ename)
{
  return rlookup(lut_rep, numof(lut_rep), ename);
}

static int snd_code(const char *ename)
{
  return rlookup(lut_snd, numof(lut_snd), ename);
}

static int sw_code(const char *ename)
{
  return rlookup(lut_sw, numof(lut_sw), ename);
}

static int ff_code(const char *ename)
{
  return rlookup(lut_ff, numof(lut_ff), ename);
}

static int pwr_code(const char *ename)
{
  (void)ename; // not used
  return -1;
}

/** Lookup input event code by name
 *
 * @param etype input event type
 * @param ename input event name
 *
 * @return input event code, or -1 in case of errors
 */
int evdev_lookup_event_code(int etype, const char *ename)
{
  typedef int (*lookup_fn)(const char *);

  static const lookup_fn lut[] =
  {
    [EV_SYN] = syn_code,
    [EV_KEY] = key_code,
    [EV_REL] = rel_code,
    [EV_ABS] = abs_code,
    [EV_MSC] = msc_code,
    [EV_SW]  = sw_code,
    [EV_LED] = led_code,
    [EV_SND] = snd_code,
    [EV_REP] = rep_code,
    [EV_FF]  = ff_code,
    [EV_PWR] = pwr_code,
  };

  if( (size_t)etype < numof(lut) && lut[etype] )
  {
    return lut[etype](ename);
  }
  return -1;
}
