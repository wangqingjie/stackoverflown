QT = core
CONFIG += c++14
DEFINES += \
  QT_DEPRECATED_WARNINGS \
  QT_DISABLE_DEPRECATED_BEFORE=0x060000 \
  QT_RESTRICTED_CAST_FROM_ASCII
SOURCES = main.cpp
macx: INCLUDEPATH += /opt/local/include/eigen3
