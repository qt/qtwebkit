TEMPLATE = app

VPATH += $$_PRO_FILE_PWD_
TARGET = tst_$$TARGET

INCLUDEPATH += $$PWD
SOURCES +=  $$PWD/util.cpp

QT += testlib webkit

qtHaveModule(quick) {
    QT += qml quick quick-private
    HEADERS += $$PWD/bytearraytestdata.h \
               $$PWD/util.h

    SOURCES += $$PWD/bytearraytestdata.cpp
}
WEBKIT += wtf # For platform macros

DEFINES += TESTS_SOURCE_DIR=\\\"$$PWD\\\" \
           QWP_PATH=\\\"$${ROOT_BUILD_DIR}/bin\\\"
