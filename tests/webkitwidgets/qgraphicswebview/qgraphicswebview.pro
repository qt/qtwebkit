include(../tests.pri)
exists($${TARGET}.qrc):RESOURCES += $${TARGET}.qrc
qtHaveModule(opengl): QT += opengl
