#include "RuntimeFeedback.h"
#include "R/r.h"

namespace rir {
RecordedType::RecordedType(SEXP s)
    : sexptype((uint8_t)TYPEOF(s)), scalar(IS_SCALAR(s, TYPEOF(s))),
      object(OBJECT(s)), attribs(ATTRIB(s) != R_NilValue) {}
} // namespace rir
