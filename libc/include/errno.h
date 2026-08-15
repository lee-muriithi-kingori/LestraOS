/*
 * Lestra OS - C Standard Library - errno (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * POSIX errno constants and the global `errno` lvalue.
 *
 * The kernel syscall ABI returns negative errno values directly (e.g.
 * -ENOENT).  libc wrappers translate that to "return -1 and set errno"
 * per POSIX.  See libc/src/errno.c for the definition of the `errno`
 * global and the strerror()/perror() helpers.
 *
 * NOTE: some legacy kernel headers (lestra/types.h) define a handful of
 * errno-like constants with NEGATIVE values for internal use.  We undef
 * those here so the canonical POSIX (positive) values take effect
 * whenever <errno.h> is included by libc or user code.  This is safe
 * because the kernel syscall dispatcher (kernel/syscall/syscall.c) uses
 * #ifndef guards around its own errno #defines and will pick up these
 * POSIX values when its build includes this header transitively.
 */
#ifndef LIBC_ERRNO_H
#define LIBC_ERRNO_H

/* Clear any stale legacy (negative) definitions so the canonical POSIX
 * values below are always used.  These are no-ops if nothing previously
 * defined the names. */
#undef EPERM
#undef ENOENT
#undef ESRCH
#undef EINTR
#undef EIO
#undef ENXIO
#undef E2BIG
#undef ENOEXEC
#undef EBADF
#undef ECHILD
#undef EAGAIN
#undef ENOMEM
#undef EACCES
#undef EFAULT
#undef ENOTBLK
#undef EBUSY
#undef EEXIST
#undef EXDEV
#undef ENODEV
#undef ENOTDIR
#undef EISDIR
#undef EINVAL
#undef ENFILE
#undef EMFILE
#undef ENOTTY
#undef ETXTBSY
#undef EFBIG
#undef ENOSPC
#undef ESPIPE
#undef EROFS
#undef EMLINK
#undef EPIPE
#undef EDOM
#undef ERANGE
#undef EDEADLK
#undef ENAMETOOLONG
#undef ENOLCK
#undef ENOSYS
#undef ENOTEMPTY
#undef ELOOP
#undef ENOMSG
#undef EIDRM
#undef ECHRNG
#undef EL2NSYNC
#undef EL3HLT
#undef EL3RST
#undef ELNRNG
#undef EUNATCH
#undef ENOCSI
#undef EL2HLT
#undef EBADE
#undef EBADR
#undef EXFULL
#undef ENOANO
#undef EBADRQC
#undef EBADSLT
#undef EBFONT
#undef ENOSTR
#undef ENODATA
#undef ETIME
#undef ENOSR
#undef ENONET
#undef ENOPKG
#undef EREMOTE
#undef ENOLINK
#undef EADV
#undef ESRMNT
#undef ECOMM
#undef EPROTO
#undef EMULTIHOP
#undef EDOTDOT
#undef EBADMSG
#undef EOVERFLOW
#undef ENOTUNIQ
#undef EBADFD
#undef EREMCHG
#undef ELIBACC
#undef ELIBBAD
#undef ELIBSCN
#undef ELIBMAX
#undef ELIBEXEC
#undef EILSEQ
#undef ERESTART
#undef ESTRPIPE
#undef EUSERS
#undef ENOTSOCK
#undef EDESTADDRREQ
#undef EMSGSIZE
#undef EPROTOTYPE
#undef ENOPROTOOPT
#undef EPROTONOSUPPORT
#undef ESOCKTNOSUPPORT
#undef EOPNOTSUPP
#undef EPFNOSUPPORT
#undef EAFNOSUPPORT
#undef EADDRINUSE
#undef EADDRNOTAVAIL
#undef ENETDOWN
#undef ENETUNREACH
#undef ENETRESET
#undef ECONNABORTED
#undef ECONNRESET
#undef ENOBUFS
#undef EISCONN
#undef ENOTCONN
#undef ESHUTDOWN
#undef ETOOMANYREFS
#undef ETIMEDOUT
#undef ECONNREFUSED
#undef EHOSTDOWN
#undef EHOSTUNREACH
#undef EALREADY
#undef EINPROGRESS
#undef ESTALE
#undef EUCLEAN
#undef ENOTNAM
#undef ENAVAIL
#undef EISNAM
#undef EREMOTEIO
#undef EDQUOT
#undef ENOMEDIUM
#undef EMEDIUMTYPE
#undef ECANCELED
#undef ENOKEY
#undef EKEYEXPIRED
#undef EKEYREVOKED
#undef EKEYREJECTED
#undef EOWNERDEAD
#undef ENOTRECOVERABLE

/* POSIX errno values (matches asm-generic/errno-base.h + errno.h). */
#define EPERM             1   /* Operation not permitted */
#define ENOENT            2   /* No such file or directory */
#define ESRCH             3   /* No such process */
#define EINTR             4   /* Interrupted system call */
#define EIO               5   /* Input/output error */
#define ENXIO             6   /* No such device or address */
#define E2BIG             7   /* Argument list too long */
#define ENOEXEC           8   /* Exec format error */
#define EBADF             9   /* Bad file descriptor */
#define ECHILD           10   /* No child processes */
#define EAGAIN           11   /* Resource temporarily unavailable */
#define ENOMEM           12   /* Cannot allocate memory */
#define EACCES           13   /* Permission denied */
#define EFAULT           14   /* Bad address */
#define ENOTBLK          15   /* Block device required */
#define EBUSY            16   /* Device or resource busy */
#define EEXIST           17   /* File exists */
#define EXDEV            18   /* Invalid cross-device link */
#define ENODEV           19   /* No such device */
#define ENOTDIR          20   /* Not a directory */
#define EISDIR           21   /* Is a directory */
#define EINVAL           22   /* Invalid argument */
#define ENFILE           23   /* Too many open files in system */
#define EMFILE           24   /* Too many open files */
#define ENOTTY           25   /* Inappropriate ioctl for device */
#define ETXTBSY          26   /* Text file busy */
#define EFBIG            27   /* File too large */
#define ENOSPC           28   /* No space left on device */
#define ESPIPE           29   /* Illegal seek */
#define EROFS            30   /* Read-only file system */
#define EMLINK           31   /* Too many links */
#define EPIPE            32   /* Broken pipe */
#define EDOM             33   /* Numerical argument out of domain */
#define ERANGE           34   /* Numerical result out of range */
#define EDEADLK          35   /* Resource deadlock avoided */
#define ENAMETOOLONG     36   /* File name too long */
#define ENOLCK           37   /* No locks available */
#define ENOSYS           38   /* Function not implemented */
#define ENOTEMPTY        39   /* Directory not empty */
#define ELOOP            40   /* Too many levels of symbolic links */
#define ENOMSG           42   /* No message of desired type */
#define EIDRM            43   /* Identifier removed */
#define ECHRNG           44   /* Channel number out of range */
#define EL2NSYNC         45   /* Level 2 not synchronized */
#define EL3HLT           46   /* Level 3 halted */
#define EL3RST           47   /* Level 3 reset */
#define ELNRNG           48   /* Link number out of range */
#define EUNATCH          49   /* Protocol driver not attached */
#define ENOCSI           50   /* No CSI structure available */
#define EL2HLT           51   /* Level 2 halted */
#define EBADE            52   /* Invalid exchange */
#define EBADR            53   /* Invalid request descriptor */
#define EXFULL           54   /* Exchange full */
#define ENOANO           55   /* No anode */
#define EBADRQC          56   /* Invalid request code */
#define EBADSLT          57   /* Invalid slot */
#define EBFONT           59   /* Bad font file format */
#define ENOSTR           60   /* Device not a stream */
#define ENODATA          61   /* No data available */
#define ETIME            62   /* Timer expired */
#define ENOSR            63   /* Out of streams resources */
#define ENONET           64   /* Machine is not on the network */
#define ENOPKG           65   /* Package not installed */
#define EREMOTE          66   /* Object is remote */
#define ENOLINK          67   /* Link has been severed */
#define EADV             68   /* Advertise error */
#define ESRMNT           69   /* Srmount error */
#define ECOMM            70   /* Communication error on send */
#define EPROTO           71   /* Protocol error */
#define EMULTIHOP        72   /* Multihop attempted */
#define EDOTDOT          73   /* RFS specific error */
#define EBADMSG          74   /* Not a data message */
#define EOVERFLOW        75   /* Value too large for defined data type */
#define ENOTUNIQ         76   /* Name not unique on network */
#define EBADFD           77   /* File descriptor in bad state */
#define EREMCHG          78   /* Remote address changed */
#define ELIBACC          79   /* Can not access a needed shared library */
#define ELIBBAD          80   /* Accessing a corrupted shared library */
#define ELIBSCN          81   /* .lib section in a.out corrupted */
#define ELIBMAX          82   /* Attempting to link in too many shared libraries */
#define ELIBEXEC         83   /* Cannot exec a shared library directly */
#define EILSEQ           84   /* Invalid or incomplete multibyte or wide character */
#define ERESTART         85   /* Interrupted system call should be restarted */
#define ESTRPIPE         86   /* Streams pipe error */
#define EUSERS           87   /* Too many users */
#define ENOTSOCK         88   /* Socket operation on non-socket */
#define EDESTADDRREQ     89   /* Destination address required */
#define EMSGSIZE         90   /* Message too long */
#define EPROTOTYPE       91   /* Protocol wrong type for socket */
#define ENOPROTOOPT      92   /* Protocol not available */
#define EPROTONOSUPPORT  93   /* Protocol not supported */
#define ESOCKTNOSUPPORT  94   /* Socket type not supported */
#define EOPNOTSUPP       95   /* Operation not supported */
#define EPFNOSUPPORT     96   /* Protocol family not supported */
#define EAFNOSUPPORT     97   /* Address family not supported by protocol */
#define EADDRINUSE       98   /* Address already in use */
#define EADDRNOTAVAIL    99   /* Cannot assign requested address */
#define ENETDOWN        100   /* Network is down */
#define ENETUNREACH     101   /* Network is unreachable */
#define ENETRESET       102   /* Network dropped connection on reset */
#define ECONNABORTED    103   /* Software caused connection abort */
#define ECONNRESET      104   /* Connection reset by peer */
#define ENOBUFS         105   /* No buffer space available */
#define EISCONN         106   /* Transport endpoint is already connected */
#define ENOTCONN        107   /* Transport endpoint is not connected */
#define ESHUTDOWN       108   /* Cannot send after transport endpoint shutdown */
#define ETOOMANYREFS    109   /* Too many references: cannot splice */
#define ETIMEDOUT       110   /* Connection timed out */
#define ECONNREFUSED    111   /* Connection refused */
#define EHOSTDOWN       112   /* Host is down */
#define EHOSTUNREACH    113   /* No route to host */
#define EALREADY        114   /* Operation already in progress */
#define EINPROGRESS     115   /* Operation now in progress */
#define ESTALE          116   /* Stale file handle */
#define EUCLEAN         117   /* Structure needs cleaning */
#define ENOTNAM         118   /* Not a XENIX named type file */
#define ENAVAIL         119   /* No XENIX semaphores available */
#define EISNAM          120   /* Is a named type file */
#define EREMOTEIO       121   /* Remote I/O error */
#define EDQUOT          122   /* Quota exceeded */
#define ENOMEDIUM       123   /* No medium found */
#define EMEDIUMTYPE     124   /* Wrong medium type */
#define ECANCELED       125   /* Operation canceled */
#define ENOKEY          126   /* Required key not available */
#define EKEYEXPIRED     127   /* Key has expired */
#define EKEYREVOKED     128   /* Key has been revoked */
#define EKEYREJECTED    129   /* Key was rejected by service */
#define EOWNERDEAD      130   /* Owner died */
#define ENOTRECOVERABLE 131   /* State not recoverable */

/* The global errno lvalue.  Defined in libc/src/errno.c.  LestraOS is
 * single-threaded per process, so a plain global is sufficient (no
 * __errno_location() indirection needed).  The `#define errno errno`
 * below marks the name as reserved and prevents any local macro from
 * shadowing it unexpectedly. */
extern int errno;
#define errno errno

/* Helper utilities — implemented in libc/src/errno.c. */
char* strerror(int errnum);
void perror(const char* s);

#endif /* LIBC_ERRNO_H */
