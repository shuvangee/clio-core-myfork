#!/bin/bash

CTP_ROOT=$1

cpplint --recursive \
--exclude="${CTP_ROOT}/include/clio_ctp/constants/singleton_macros.h" \
--exclude="${CTP_ROOT}/include/clio_ctp/data_structures/internal/template" \
--exclude="${CTP_ROOT}/include/clio_ctp/data_structures/internal/shm_container_macro.h" \
--exclude="${CTP_ROOT}/src/singleton.cc" \
--exclude="${CTP_ROOT}/src/data_structure_singleton.cc" \
--exclude="${CTP_ROOT}/include/clio_ctp/util/formatter.h" \
--exclude="${CTP_ROOT}/include/clio_ctp/util/errors.h" \
"${CTP_ROOT}/src" "${CTP_ROOT}/include" "${CTP_ROOT}/test"