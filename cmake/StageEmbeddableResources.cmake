# Invoked by the embeddable target after copy_resources (and optionally web-app).
if(NOT IS_DIRECTORY "${RESOURCE_SOURCE}")
    message(FATAL_ERROR "Embeddable runtime resources are missing: ${RESOURCE_SOURCE}")
endif()
if(NOT DEFINED RESOURCE_DESTINATION OR RESOURCE_DESTINATION STREQUAL "")
    message(FATAL_ERROR "RESOURCE_DESTINATION is required")
endif()
if(INCLUDE_WEB_APP AND NOT EXISTS "${RESOURCE_SOURCE}/web-app/index.html")
    message(FATAL_ERROR "BUILD_WEB_APP is enabled but built web-app/index.html is missing; build the web-app target first")
endif()

# A previous web build may still be present when BUILD_WEB_APP is turned OFF.
# Do not ship it implicitly in the service-only archive.
file(COPY "${RESOURCE_SOURCE}/" DESTINATION "${RESOURCE_DESTINATION}"
    PATTERN "web-app" EXCLUDE)
if(INCLUDE_WEB_APP)
    file(COPY "${RESOURCE_SOURCE}/web-app" DESTINATION "${RESOURCE_DESTINATION}")
endif()
