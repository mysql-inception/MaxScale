# Build the MariaDB Connector-C
#
# If the MariaDB connector is not found, the last option is to download it
# and build it from source. This file downloads and builds the connector and
# sets the variables set by FindMariaDBConnector.cmake so that it appears that
# the system has the connector.

include(ExternalProject)

set(MARIADB_CONNECTOR_C_REPO "https://github.com/mysql-inception/mariadb-connector-c.git"
  CACHE STRING "MariaDB Connector-C Git repository")

# Release 2.2.3 of the Connector-C
set(MARIADB_CONNECTOR_C_TAG "v2.3.0"
  CACHE STRING "MariaDB Connector-C Git tag")

ExternalProject_Add(connector-c
  GIT_REPOSITORY ${MARIADB_CONNECTOR_C_REPO}
  GIT_TAG ${MARIADB_CONNECTOR_C_TAG}
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/connector-c/install
  BINARY_DIR ${CMAKE_BINARY_DIR}/connector-c
  INSTALL_DIR ${CMAKE_BINARY_DIR}/connector-c/install
  UPDATE_COMMAND "")

set(CMAKE_PREFIX_PATH "/usr/local/ssl" ${CMAKE_PREFIX_PATH})
set(MARIADB_CONNECTOR_FOUND TRUE CACHE INTERNAL "")
set(MARIADB_CONNECTOR_STATIC_FOUND TRUE CACHE INTERNAL "")
set(MARIADB_CONNECTOR_INCLUDE_DIR
  ${CMAKE_BINARY_DIR}/connector-c/install/include/mariadb CACHE INTERNAL "")
set(MARIADB_CONNECTOR_STATIC_LIBRARIES
  ${CMAKE_BINARY_DIR}/connector-c/install/lib/mariadb/libmariadbclient.a
  CACHE INTERNAL "")
set(MARIADB_CONNECTOR_LIBRARIES
  ${CMAKE_BINARY_DIR}/connector-c/install/lib/mariadb/libmariadbclient.a
  CACHE INTERNAL "")
