// AppMain.h — shared start-up code of both panels: single instance, --minimized, theme.
#pragma once

#include <QApplication>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStringList>
#include <QSystemTrayIcon>

#include "BreezeTheme.h"

template <class Window>
int RunPanel(int argc, char** argv)
{
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setApplicationName(QStringLiteral("AC3 Control Center"));
  app.setFont(QFont(QStringLiteral("Segoe UI"), 9));

  const bool hidden = app.arguments().contains(QStringLiteral("--minimized"));
  const QString serverName = QStringLiteral("Ac3ControlCenterQt.SingleInstance");

  {
    // A panel is already running: just ask it to show itself (not at logon).
    QLocalSocket probe;
    probe.connectToServer(serverName);
    if (probe.waitForConnected(300))
    {
      if (!hidden)
      {
        probe.write("show");
        probe.waitForBytesWritten(300);
      }
      return 0;
    }
  }
  QLocalServer::removeServer(serverName);
  QLocalServer server;
  server.listen(serverName);

  BreezeTheme::Apply(app, true);  // the window switches to the saved theme from control-center.ini
  Window window(hidden);
  QObject::connect(&server, &QLocalServer::newConnection, &window, [&]() {
    while (QLocalSocket* client = server.nextPendingConnection()) client->deleteLater();
    window.showPanel();
  });
  window.startup();
  if (!hidden || !QSystemTrayIcon::isSystemTrayAvailable()) window.show();
  return app.exec();
}
