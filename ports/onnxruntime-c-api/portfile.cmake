# onnxruntime-c-api — the ONNX Runtime C API header, and nothing else.
#
# NOT the stock `onnxruntime` port, and deliberately not named as it.
# That port builds the whole runtime from source with twenty-two
# dependencies -- abseil, protobuf, eigen, onnx -- and we would link none
# of it. SkyrimNet already ships an ONNX Runtime and already has it
# loaded in the process; this plugin binds to that copy through
# `OrtGetApiBase`, so all it needs from Microsoft is the declarations.
#
# Shadowing the name `onnxruntime` would have worked too, and would have
# been a trap: anything later wanting the real runtime would silently get
# a header with no library behind it.
#
# VERSION. The header pins ORT_API_VERSION, and a runtime serves its own
# API version and every OLDER one. SkyrimNet ships 1.26.0, which reports
# serving 1 through 26, so 1.26.0 is the highest header that works today
# and any lower one would also work. Pinning at or below their version is
# what keeps this durable: their updates can only raise the ceiling.
#
# If SkyrimNet ever ships an OLDER runtime, this stops resolving and says
# so at startup rather than crashing -- see CharacterEmbedding's
# negotiation against GetApi().

vcpkg_download_distfile(
  HEADER
  URLS "https://raw.githubusercontent.com/microsoft/onnxruntime/refs/tags/v${VERSION}/include/onnxruntime/core/session/onnxruntime_c_api.h"
  FILENAME "onnxruntime_c_api-${VERSION}.h"
  SHA512 4027dbbce7d315e14ddb35c1f1e80eb6c9771e170fb27edaf0034d1c05cfc736b5820293160033698e3b9ab1ce09f48da87518ced59477694009ee6d07faf20c
)

# The C API header includes this one at its tail, and that one includes
# the first back -- a closed pair, and the only two files needed. Worth
# stating because the obvious assumption is that a "C API header" is one
# file, and building on that assumption fails at the last line of the
# first header rather than anywhere useful.
vcpkg_download_distfile(
  EP_HEADER
  URLS "https://raw.githubusercontent.com/microsoft/onnxruntime/refs/tags/v${VERSION}/include/onnxruntime/core/session/onnxruntime_ep_c_api.h"
  FILENAME "onnxruntime_ep_c_api-${VERSION}.h"
  SHA512 64456fe2c3d47dda0c9997fd8398f3fd0f5502c01b2103bd7887c2ddaf8f1db0b83622661bcdd497d92a003448193b8be9352728356562ee88cc80c91eb8818e
)

vcpkg_download_distfile(
  LICENSE_FILE
  URLS "https://raw.githubusercontent.com/microsoft/onnxruntime/refs/tags/v${VERSION}/LICENSE"
  FILENAME "onnxruntime-LICENSE-${VERSION}"
  SHA512 2803e85846b04fb0b873c7f324df43113ad7a682bf58824d6f74d431db2242f375e986c6fe13c96a61964bb3724d0d87441a452e300af99c65f8951a8df10e11
)

file(
    INSTALL "${HEADER}"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include"
    RENAME
    onnxruntime_c_api.h
)

file(
    INSTALL "${EP_HEADER}"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include"
    RENAME
    onnxruntime_ep_c_api.h
)

vcpkg_install_copyright(FILE_LIST "${LICENSE_FILE}")

# Nothing is compiled and nothing is linked, so vcpkg should not go
# looking for libraries that were never going to be there.
set(VCPKG_POLICY_HEADER_ONLY enabled)
