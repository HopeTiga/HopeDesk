#pragma once

// Ref-counting for the AV1 DXVA port reuses WebRTC's scoped_refptr /
// RefCountInterface / RefCountedObject (the project already links WebRTC).
// No using/typedef aliases: always write webrtc::scoped_refptr fully qualified.
#include "api/scoped_refptr.h"
#include "api/ref_count.h"
#include "rtc_base/ref_counted_object.h"
