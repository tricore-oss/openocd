#ifndef IFXDAP_H
#define IFXDAP_H
#include <jtag/interface.h>

struct ifxdap_driver {
  int (*init)(void);
};

#endif /* IFXDAP_H */