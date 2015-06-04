# included mk file for the openssl module

OPENSSL_DIR := ${MODULE_DIR}/openssl
OPENSSL_CONFIGURE := ${OPENSSL_DIR}/config
OPENSSL_CONFIG_MARKER := .openssl_config_run
OPENSSL_MAKEFILE := ${OPENSSL_DIR}/Makefile

${OPENSSL_CONFIG_MARKER}: 
	cd ${OPENSSL_DIR}; \
	./config --prefix=${INSTALL_DIR} -fPIC
	touch ${OPENSSL_CONFIG_MARKER}

openssl: ${OPENSSL_CONFIG_MARKER}
	${MAKE} -C ${OPENSSL_DIR}
	# We can't make 'install' because it makes target 'install_docs'
	# which doesn't build properly using Ubuntu 14.04 tools. As such,
	# we'll just make it and only install the software.
	${MAKE} -C ${OPENSSL_DIR} install_sw

openssl_test: ${OPENSSL_CONFIG_MARKER}
	${MAKE} -C ${OPENSSL_DIR} test

openssl_clean: ${OPENSSL_CONFIG_MARKER}
	${MAKE} -C ${OPENSSL_DIR} clean

openssl_distclean: openssl_clean
	rm -f ${OPENSSL_CONFIG_MARKER}

.PHONY: openssl openssl_test openssl_clean openssl_distclean
