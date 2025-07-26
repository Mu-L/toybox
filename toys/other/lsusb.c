/* lsusb.c - list available USB devices
 *
 * Copyright 2013 Andre Renaud <andre@bluewatersys.com>
 * Copyright 2013 Isaac Dunham <ibid.ag@gmail.com>

USE_LSUSB(NEWTOY(lsusb, "ti:", TOYFLAG_USR|TOYFLAG_BIN))
USE_LSPCI(NEWTOY(lspci, "eDmkn@x@i:", TOYFLAG_USR|TOYFLAG_BIN))

config LSPCI
  bool "lspci"
  default y
  help
    usage: lspci [-ekmn] [-i FILE]

    List PCI devices.

    -e	Extended (6 digit) class
    -i	ID database (default /etc/pci.ids[.gz])
    -k	Show kernel driver
    -m	Machine readable
    -n	Numeric output (-nn for both)
    -D	Print domain numbers
    -x	Hex dump of config space (64 bytes; -xxx for 256, -xxxx for 4096)

config LSUSB
  bool "lsusb"
  default y
  help
    usage: lsusb [-ti]

    List USB hosts/devices.

    -t	Tree format
    -i	ID database (default /etc/usb.ids[.gz])
*/

#define FOR_lsusb
#include "toys.h"

GLOBALS(
  char *i;
  long x, n;

  void *ids, *class;
  int count;
)

// Structures for tree display
struct usb_device {
  struct usb_device *next, *child;
  unsigned busnum, devnum, portnum;
  unsigned vid, pid;
  char manufacturer[64], product[64], speed[16];
  char name[];
};

struct usb_bus {
  struct usb_bus *next;
  struct usb_device *first_child;
  unsigned busnum;
  char name[64];
};

struct dev_ids {
  struct dev_ids *next, *child;
  int id;
  char name[];
};

struct scanloop {
  char *pattern;
  void *d1, *d2;
};

// Common function to read uevent file under /proc for both pci and usb
static int scan_uevent(struct dirtree *new, int len, struct scanloop *sl)
{
  int ii, saw = 0;
  off_t flen = sizeof(toybuf);
  char *ss = toybuf, *yy;

  // Read data
  if (*new->name == '.') return 0;
  sprintf(ss, "%s/uevent", new->name);
  if (!readfileat(dirtree_parentfd(new), ss, ss, &flen)) return 0;

  // Loop over lines
  while ((flen = strcspn(ss, "\n"))) {
    if (ss[flen]) ss[flen++] = 0;
    yy = ss+flen;

    // Try each pattern
    for (ii = 0; ii<len; ii++) {
      if (strchr(sl[ii].pattern, '%')) {
        if (2-!sl[ii].d2!=sscanf(ss, sl[ii].pattern, sl[ii].d1, sl[ii].d2))
          continue;
      } else if (strstart(&ss, sl[ii].pattern)) *(void **)sl[ii].d1 = ss;
      else continue;
      saw |= 1<<ii;

      break;
    }
    ss = yy;
  }

  return saw;
}

static void get_names(struct dev_ids *ids, int id1, int id2,
  char **name1, char **name2)
{
  // Look up matching dev_ids (if any)
  *name1 = *name2 = "";
  for (; ids; ids = ids->next) {
    if (id1 != ids->id) continue;
    *name1 = ids->name;
    for (ids = ids->child; ids; ids = ids->next) {
      if (id2 != ids->id) continue;
      *name2 = ids->name;
      return;
    }
    return;
  }
}

// Search for pci.ids or usb.ids and return parsed structure or NULL
struct dev_ids *parse_dev_ids(char *name, struct dev_ids **and)
{
  char *path = "/etc:/vendor:/usr/share/hwdata:/usr/share/misc";
  struct string_list *sl = 0;
  FILE *fp;
  char *s, *ss, *sss;
  struct dev_ids *ids = 0, *new;
  int fd = -1;

  // Open compressed or uncompressed file
  signal(SIGCHLD, SIG_IGN);
  s = TT.i;
  if (!s) {
    sprintf(toybuf, "%s.gz", name);
    if ((sl = find_in_path(path, toybuf)) || (sl = find_in_path(path, name)))
      s = sl->str;
  }
  if (s && strend(s, ".gz")) xpopen((char *[]){"zcat", sl->str, 0}, &fd, 1);
  else if (s) fd = xopen(s, O_RDONLY);
  llist_traverse(sl, free);
  if (fd == -1) return 0;

  for (fp = fdopen(fd, "r"); (s = ss = xgetline(fp)); free(s)) {
    // TODO parse and use third level instead of skipping it here
    if (s[strspn(s, " \t")]=='#' || strstart(&ss, "\t\t")) continue;

    // Switch to device class list?
    if (strstart(&ss, "C ") && and) {
      *and = ids;
      and = 0;
    }
    fd = estrtol(sss = ss, &ss, 16);
    if (ss>sss && *ss++==' ') {
      while (isspace(*ss)) ss++;
      new = xmalloc(sizeof(*new)+strlen(ss)+1);
      new->child = 0;
      new->id = fd;
      strcpy(new->name, ss);
      if (!ids || *s!='\t') {
        new->next = ids;
        ids = new;
      } else {
        new->next = ids->child;
        ids->child = new;
      }
    }
  }
  fclose(fp);

  return ids;
}

static int list_usb(struct dirtree *new)
{
  int busnum = 0, devnum = 0, pid = 0, vid = 0;
  char *n1, *n2;

  if (!new->parent) return DIRTREE_RECURSE;
  if (7 == scan_uevent(new, 3, (struct scanloop[]){{"BUSNUM=%u", &busnum, 0},
    {"DEVNUM=%u", &devnum, 0}, {"PRODUCT=%x/%x", &pid, &vid}}))
  {
    get_names(TT.ids, pid, vid, &n1, &n2);
    printf("Bus %03d Device %03d: ID %04x:%04x %s %s\n",
      busnum, devnum, pid, vid, n1, n2);
  }

  return 0;
}

// Tree display functions
static struct usb_bus *usb_buses = NULL;

static char *readat(int dir, char *name)
{
  off_t len = sizeof(toybuf);

  return readfileat(dir, name, toybuf, &len) ? : "";
}

static struct usb_device *create_device(struct dirtree *node)
{
  int fd = openat(dirtree_parentfd(node), node->name, O_RDONLY);
  struct usb_device *dev;

  dev = xzalloc(sizeof(struct usb_device)+strlen(node->name)+1);
  strcpy(dev->name, node->name);

  // Parse device path to get busnum, devnum, portnum
  sscanf(node->name, "%u-%u", &dev->busnum, &dev->portnum);
  dev->devnum = strtoul(readat(fd, "devnum"), 0, 10);
  dev->vid = strtoul(readat(fd, "idVendor"), 0, 16);
  dev->pid = strtoul(readat(fd, "idProduct"), 0, 16);
  strncpy(dev->manufacturer, chomp(readat(fd, "manufacturer")),
    sizeof(dev->manufacturer));
  strncpy(dev->product, chomp(readat(fd, "product")), sizeof(dev->product));
  strncpy(dev->speed, chomp(readat(fd, "speed")), sizeof(dev->speed));

  return dev;
}

static struct usb_bus *find_or_create_bus(unsigned int busnum)
{
  struct usb_bus *bus, **pp;

  for (pp = &usb_buses; (bus = *pp); pp = &bus->next)
    if (bus->busnum == busnum) return bus;

  bus = xzalloc(sizeof(*bus));
  bus->busnum = busnum;
  snprintf(bus->name, sizeof(bus->name), "usb%u", busnum);
  *pp = bus;

  return bus;
}

static void add_device_to_tree(struct usb_device *dev)
{
  struct usb_bus *bus;
  struct usb_device **pp;

  bus = find_or_create_bus(dev->busnum);

  // For simplicity, add all devices to bus level
  if (!bus->first_child) {
    bus->first_child = dev;
  } else {
    for (pp = &bus->first_child; *pp; pp = &(*pp)->next);
    *pp = dev;
  }
}

static int scan_usb_devices_tree(struct dirtree *node)
{
  if (!node->parent) return DIRTREE_RECURSE;
  if (!strchr(node->name, ':'))
    if (isdigit(*node->name) || !strncmp(node->name, "usb", 3)) 
      add_device_to_tree(create_device(node));

  return 0;
}

static void print_device_tree(struct usb_device *dev, int indent)
{
  char *vendor = "", *product = "";

  for (;dev; dev = dev->next) {
    if (TT.ids) get_names(TT.ids, dev->vid, dev->pid, &vendor, &product);

    printf("%*s|__ Port %u: Dev %u, If 0, Class=hub, Driver=hub/0p, %sM\n",
      indent, "", dev->portnum ? : 1, dev->devnum,
      *dev->speed ? dev->speed : "480");

    if (dev->vid || dev->pid || *vendor || *product)
      printf("%*s    ID %04x:%04x %s %s\n", indent, "", dev->vid, dev->pid,
        vendor, product);

    if (dev->child) print_device_tree(dev->child, indent + 4);
  }
}

void lsusb_main(void)
{
  // Parse http://www.linux-usb.org/usb.ids file (if available)
  TT.ids = parse_dev_ids("usb.ids", 0);

  if (FLAG(t)) {
    dirtree_read("/sys/bus/usb/devices/", scan_usb_devices_tree);
  } else dirtree_read("/sys/bus/usb/devices/", list_usb);
  if (FLAG(t)) {
    struct usb_bus *bus;

    for (bus = usb_buses; bus; bus = bus->next) {
      printf("/:  Bus %02u.Port 1: Dev 1, Class=root_hub, Driver=hub/0p, 480M\n", bus->busnum);
      if (bus->first_child) print_device_tree(bus->first_child, 4);
    }
  }
}

#define FOR_lspci
#include "generated/flags.h"

// TODO: -v
static int list_pci(struct dirtree *new)
{
  char *driver = 0, buf[16], *ss = toybuf, *names[3];
  int cvd[3] = {0}, ii, revision = 0;
  off_t len = sizeof(toybuf);
  /* skip 0000: part by default */
  char *bus = strchr(new->name, ':') + 1;

// Output formats: -n, -nn, -m, -nm, -nnm, -k

  if (!new->parent) return DIRTREE_RECURSE;
  if (!bus || strlen(new->name)<6) return 0;
  TT.count = 0;

  // Load revision
  sprintf(ss, "%s/revision", new->name);
  if (readfileat(dirtree_parentfd(new), ss, ss, &len)) {
    strstart(&ss, "0x");
    sscanf(ss, "%x", &revision);
  }

  // Load uevent data, look up names in database
  if (6>scan_uevent(new, 3, (struct scanloop[]){{"DRIVER=", &driver, 0},
    {"PCI_CLASS=%x", cvd, 0}, {"PCI_ID=%x:%x", cvd+1, cvd+2}})) return 0;
  get_names(TT.class, 255&(cvd[0]>>16), 255&(cvd[0]>>8), names, names);
  get_names(TT.ids, cvd[1], cvd[2], names+1, names+2);
  if (!FLAG(e)) cvd[0] >>= 8;

  // Output line according to flags
  if (FLAG(D) || strncmp(new->name, "0000:", bus-new->name)) bus = new->name;
  printf("%s", bus);
  for (ii = 0; ii<3; ii++) {
    sprintf(buf, "%0*x", 6-2*(ii||!FLAG(e)), cvd[ii]);
    if (!TT.n) printf(FLAG(m) ? " \"%s\"" : ": %s"+(ii!=1), names[ii] ? : buf);
    else if (TT.n==1) printf(FLAG(m) ? " \"%s\"" : (ii==2)?"%s ":" %s:", buf);
    else if (!FLAG(m)) {
      // This one permutes the order, so do it all first time and abort loop
      printf(" %s [%s]: %s %s [%04x:%04x]", names[0], buf, names[1], names[2],
        cvd[1], cvd[2]);
      break;
    } else printf(" \"%s [%s]\"", names[ii], buf);
  }
  if (revision) printf(FLAG(m) ? " -r%02x" : " (rev %02x)", revision);
  if (FLAG(k) && driver) printf(FLAG(m) ? " \"%s\"" : " %s", driver);
  xputc('\n');

  if (TT.x) {
    FILE *fp;
    int b, col = 0, max = (TT.x >= 4) ? 4096 : ((TT.x >= 3) ? 256 : 64);

    snprintf(toybuf, sizeof(toybuf), "/sys/bus/pci/devices/%s/config", new->name);
    fp = xfopen(toybuf, "r");
    while ((b = fgetc(fp)) != EOF) {
      if ((col % 16) == 0) printf("%02x: ", col & ~0xf);
      printf("%02x ", (b & 0xff));
      if ((++col % 16) == 0) xputc('\n');
      if (col == max) break;
    }
    xputc('\n');
    fclose(fp);
  }

  return 0;
}

void lspci_main(void)
{
  // Parse https://pci-ids.ucw.cz/v2.2/pci.ids (if available)
  if (TT.n != 1) TT.class = parse_dev_ids("pci.ids", (void *)&TT.ids);
  dirtree_read("/sys/bus/pci/devices/", list_pci);
}
