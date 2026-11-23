#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The `docs` target: the API reference and the concept guides, by Doxygen, a build tool only
# (never a library dependency). Warnings are errors, so an undocumented public declaration in a
# header held to the gate fails the build. A header joins DANNAN_DOC_HEADERS once every public
# declaration in it is documented (the backfill's order is in TODO/ACTIVE.md); from 0.12.0 every
# header a release adds or changes is on it before the release is tagged.

find_package(Doxygen 1.9 REQUIRED)

set(DANNAN_DOC_HEADERS
  agree/agreement.h
  config/config.h
  config/node_settings.h
  context.h
  contract/completion.h
  contract/hlc.h
  contract/time_source.h
  contract/time_wait.h
  identity/password.h
  listen/backoff.h
  listen/rate_limiter.h
  node.h
  net/fanout.h
  net/scheduler.h
  placement/placement.h
  sched/buffer_pool.h
  sched/retained_memory.h
  status/readiness.h
  supervise/command.h
  supervise/node_program.h
)
set(DANNAN_DOC_GUIDES
  guide/index.md
  guide/nodes.md
  guide/membership.md
  guide/founding.md
  guide/clocks.md
  guide/delivery.md
  guide/faults.md
  guide/settings.md
  guide/namespaces.dox
)

set(inputs)
foreach(header ${DANNAN_DOC_HEADERS})
  list(APPEND inputs "${PROJECT_SOURCE_DIR}/dannan/${header}")
endforeach()
foreach(guide ${DANNAN_DOC_GUIDES})
  list(APPEND inputs "${CMAKE_CURRENT_SOURCE_DIR}/${guide}")
endforeach()
list(JOIN inputs " " DANNAN_DOC_INPUT)
set(DANNAN_DOC_OUTPUT "${PROJECT_BINARY_DIR}/docs")
configure_file(Doxyfile.in "${PROJECT_BINARY_DIR}/Doxyfile" @ONLY)

add_custom_target(docs
  COMMAND ${DOXYGEN_EXECUTABLE} "${PROJECT_BINARY_DIR}/Doxyfile"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  COMMENT "The API reference and guides, by Doxygen, warnings as errors"
  VERBATIM)
