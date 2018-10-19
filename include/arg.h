#ifdef GWUDP
typedef struct {
  m_str  host;
  int    port;
  unsigned on : 1;
} UdpIf;
#endif
typedef struct {
  int argc;
  char** argv;
  m_int loop;
  m_uint quit;
  struct Vector_ add;
  struct Vector_ rem;
  struct Vector_ lib;
  Vector ref;
#ifdef GWCOV
  unsigned coverage : 1;
#endif
} Arg;

ANN void arg_init(Arg* arg);
ANN void arg_release(Arg* arg);
