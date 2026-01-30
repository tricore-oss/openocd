#ifndef TRICORE_REGISTER_H
#define TRICORE_REGISTER_H

/* Register defines would go here based on the provided table */
/* Debug registers */
#define TRICORE_DBGSR 0xFD00
#define TRICORE_EXEVT 0xFD08
#define TRICORE_CREVT 0xFD0C
#define TRICORE_SWEVT 0xFD10
#define TRICORE_DBGACT 0xFD14
#define TRICORE_TRIG_ACC 0xFD30
#define TRICORE_DMS 0xFD40
#define TRICORE_DCX 0xFD44
#define TRICORE_DBGTCR 0xFD48
#define TRICORE_DBGCFG 0xFD4C

/* Trigger event registers */
#define TRICORE_TRxEVT(x) (0xF000 + (x) * 0x8)
#define TRICORE_TRxADR(x) (0xF004 + (x) * 0x8)

#endif