// Solo se compila en el build de ThreadSanitizer (-DENABLE_TSAN=ON).
//
// TSan llama a esta función sola al arrancar y suprime los reportes cuya pila pasa por estas
// bibliotecas: las del sistema gráfico que usa OpenCV para la ventana (Qt y su DBus en este equipo;
// GTK/GLib si OpenCV se compiló con GTK). Son condiciones de carrera DENTRO de esas bibliotecas
// (p. ej. el hilo QDBusConnection de Qt contra el hilo principal), no instrumentadas y fuera de
// nuestro control. No tapan las nuestras: un reporte se suprime solo si la pila pasa por una de
// ellas, y una carrera inyectada a propósito en nuestro código se sigue reportando (README, DP.11).
extern "C" const char* __tsan_default_suppressions()
{
    return "race:libQt5Core.so\n"
           "race:libQt5Gui.so\n"
           "race:libQt5Widgets.so\n"
           "race:libQt5DBus.so\n"
           "race:libQt5XcbQpa.so\n"
           "race:libdbus-1.so\n"
           "deadlock:libdbus-1.so\n"
           "race:libgtk-3.so\n"
           "race:libgdk-3.so\n"
           "race:libglib-2.0.so\n"
           "race:libgobject-2.0.so\n"
           "race:libgio-2.0.so\n"
           "deadlock:libglib-2.0.so\n";
}
