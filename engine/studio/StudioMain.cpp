// StudioMain.cpp — DSP Studio entry point: one instance, saved look, then the window.
#include <QApplication>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>

#include "StudioTheme.h"
#include "StudioWindow.h"

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("VirtualAc3 DSP Studio"));
  app.setFont(QFont(QStringLiteral("Segoe UI"), 9));

  const QString serverName = QStringLiteral("VirtualAc3DspStudio.SingleInstance");
  {
    // Already running: just ask that window to come to the front.
    QLocalSocket probe;
    probe.connectToServer(serverName);
    if (probe.waitForConnected(300))
    {
      probe.write("show");
      probe.waitForBytesWritten(300);
      return 0;
    }
  }
  QLocalServer::removeServer(serverName);
  QLocalServer server;
  server.listen(serverName);

  QSettings ini(QCoreApplication::applicationDirPath() + QStringLiteral("/dsp-studio.ini"), QSettings::IniFormat);
  int look = ini.value(QStringLiteral("look"), 0).toInt();
  if (look < 0 || look >= StudioTheme::kLookCount)
    look = 0;
  StudioTheme::Apply(app, static_cast<StudioTheme::Look>(look));

  StudioWindow window;
  QObject::connect(&server, &QLocalServer::newConnection, &window, [&]() {
    while (QLocalSocket* client = server.nextPendingConnection())
      client->deleteLater();
    window.showStudio();
  });
  window.show();
  return app.exec();
}
