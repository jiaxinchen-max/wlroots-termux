#ifndef WLR_API_H
#define WLR_API_H

#ifndef WLR_IMPLEMENTATION

#if __STDC_VERSION__ >= 202311L
#define WLR_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(__GNUC__)
#define WLR_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#define WLR_DEPRECATED(msg)
#endif

#else

#define WLR_DEPRECATED(msg)

#endif

#endif
