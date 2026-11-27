// Console menus transcribed from ref\sw2\walkthrough.mp4 (Switch 2, Spanish, firmware 23.5.0, docked).
//
// Conventions
//  - Labels, values and notes are the console's wording, punctuation and capitalisation.
//  - A note with several paragraphs uses '\n'; the console's bullet lines start with "◆ ".
//  - Personal data visible in the capture (user names, console nickname, friend code, Nintendo
//    Account name, network name, MAC/IP/DNS addresses, play history, friends) is NOT copied:
//    the placeholder "—" stands in its place.
//  - Toggle states and values are the ones on screen in the capture (console docked to a TV).
//  - Rows with captured == false were only partly visible; sections with complete == false were not
//    scrolled to the end or not entered.
//  - Blank gaps between groups of rows on the console are noted in comments ("gap").
#include "console_menus.h"

namespace console {
namespace {

// Button glyphs drawn inline by the console. A/B use the code points ui.cpp already uses from the
// firmware extension font. HOME is assumed to be U+E0F4 in that font (not verified here). The C
// button and the virtual game card icon have no known code point in this project: ASCII stand-ins.
#define G_A     L"\uE0E0"
#define G_B     L"\uE0E1"
#define G_HOME  L"\uE0F4"
#define G_C     L"[C]"
#define G_VCARD L"[tarjeta]"

const wchar_t* const kNone = L"";
const wchar_t* const kPersonal = L"—";   // "—": personal data left out on purpose

Row H(const wchar_t* label, const char* id) {
    return Row{RowKind::Heading, label, kNone, false, kNone, id, true};
}
Row A(const wchar_t* label, const char* id, const wchar_t* note = kNone, bool captured = true) {
    return Row{RowKind::Action, label, kNone, false, note, id, captured};
}
Row V(const wchar_t* label, const wchar_t* value, const char* id, const wchar_t* note = kNone) {
    return Row{RowKind::Value, label, value, false, note, id, true};
}
Row T(const wchar_t* label, bool on, const char* id, const wchar_t* note = kNone) {
    return Row{RowKind::Toggle, label, kNone, on, note, id, true};
}
Row S(const wchar_t* label, const char* id) {
    return Row{RowKind::Slider, label, kNone, false, kNone, id, true};
}
Row X(const wchar_t* text, const char* id, const wchar_t* note = kNone) {
    return Row{RowKind::Text, text, kNone, false, note, id, true};
}
Row P(const wchar_t* label, const char* id, const wchar_t* note) {
    return Row{RowKind::Picture, label, kNone, false, note, id, true};
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Configuración de la consola
// Left list order as on the console; thin separators after "Audio por Bluetooth®" and after
// "Gestión de datos". Footer: (B) Atrás  (A) Confirmar.
// ---------------------------------------------------------------------------------------------
const Menu& Settings() {
    static const Menu m{
        L"Configuración de la consola",
        {
            // Not scrolled: only the top of the page was on screen.
            Section{L"Ayuda", "help", {
                X(L"Si quieres consultar información más detallada sobre la consola y sus funciones, "
                  L"visita el sitio web del servicio de ayuda de Nintendo desde un ordenador o un "
                  L"dispositivo inteligente.", "help.intro"),
                P(L"support.nintendo.com", "help.qr",
                  L"Panel con una ilustración (persona con un móvil) y un código QR; debajo "
                  L"«support.nintendo.com» y, en gris: «Proveedor de servicios en línea (solo productos "
                  L"de Nintendo):\nNintendo Co., Ltd. Kioto 601-8501, Japón»"),
                A(L"Información sobre salud y seguridad", "help.health_safety"),   // warning-triangle icon before the label
                A(L"Historial de errores", "help.error_history", kNone, false),   // only the top half of the letters visible
            }, false},

            // Greyed out because the console was docked.
            Section{L"Modo avión", "airplane", {
                T(L"Modo avión", false, "airplane.enabled",
                  L"Esta función no está disponible con la consola conectada al televisor."),
            }, true},

            // Docked: only this sentence, centred, no slider.
            Section{L"Brillo de la pantalla", "brightness", {
                X(L"El brillo de la pantalla táctil no se puede regular si la consola está conectada "
                  L"al televisor.", "brightness.docked_notice"),
            }, true},

            Section{L"Audio por Bluetooth®", "bluetooth", {
                A(L"Añadir dispositivo", "bluetooth.add_device",
                  L"Es necesario volver a conectar los dispositivos para usar audio por Bluetooth.\n"
                  L"◆ Es posible conectar hasta dos mandos inalámbricos mientras se utiliza audio a través de Bluetooth.\n"
                  L"◆ El audio por Bluetooth se desconectará al usar la comunicación local.\n"
                  L"◆ No es posible utilizar micrófonos Bluetooth.\n"
                  L"◆ Puede que experimentes problemas de latencia según el dispositivo Bluetooth que utilices.\n"
                  L"◆ Es posible conectar y desconectar dispositivos mediante la configuración de acceso rápido "
                  L"(mantén pulsado " G_HOME L")."),
            }, true},

            // The status block is a striped table: first line is its title with the state on the right.
            Section{L"Internet", "internet", {
                A(L"Configuración de internet", "internet.settings"),
                A(L"Prueba de conexión", "internet.test_connection"),
                // gap
                V(L"Estado de la conexión", L"Conexión con red Wi-Fi", "internet.status"),
                V(L"Dirección MAC de la consola", kPersonal, "internet.mac"),
                V(L"Dispositivo de red", kPersonal, "internet.network_name"),
                V(L"Tipo de seguridad", L"WPA2-Personal", "internet.security"),
                V(L"Banda de frecuencia", L"5 GHz", "internet.band"),
                V(L"Canal", L"64", "internet.channel"),
                V(L"Intensidad de la señal", kNone, "internet.signal"),             // Wi-Fi bars icon instead of text
                V(L"Dirección IP (IPv4)", kPersonal, "internet.ipv4"),
                V(L"Máscara de subred", kPersonal, "internet.subnet"),
                V(L"Puerta de enlace (IPv4)", kPersonal, "internet.gateway4"),
                V(L"DNS primario (IPv4)", kPersonal, "internet.dns1_4"),
                V(L"DNS secundario (IPv4)", kPersonal, "internet.dns2_4"),
                V(L"Dirección IP (IPv6)", kPersonal, "internet.ipv6"),              // a spinner at first, then addresses
                V(L"Puerta de enlace (IPv6)", kPersonal, "internet.gateway6"),
                V(L"DNS primario (IPv6)", kPersonal, "internet.dns1_6"),
                V(L"DNS secundario (IPv6)", kPersonal, "internet.dns2_6"),
            }, true},

            Section{L"Control parental", "parental", {
                X(L"Nintendo te ofrece la posibilidad de crear un entorno de juego seguro para los niños "
                  L"mediante el control parental, que se puede configurar con esta consola o con un "
                  L"dispositivo inteligente.", "parental.intro"),                   // parental-controls icon on the left
                A(L"Ver vídeo introductorio de la aplicación Control parental de Switch",
                  "parental.intro_video"),                                           // video thumbnail on the left, label on two lines
                // gap
                A(L"Ajustes del control parental", "parental.settings"),
            }, true},

            Section{L"Accesibilidad", "accessibility", {
                A(L"Cambiar la asignación de los botones", "accessibility.remap"),
                T(L"Reasignar botones (configuración de acceso rápido)", false, "accessibility.remap_quick",
                  L"Al activar esta opción, podrás cambiar la asignación de botones directamente desde la "
                  L"configuración de acceso rápido (mantén pulsado " G_HOME L")."),
                // gap
                V(L"Tamaño del texto", L"Normal", "accessibility.text_size"),
                T(L"Texto en negrita", false, "accessibility.bold"),
                T(L"Contraste alto", false, "accessibility.high_contrast"),
                T(L"Acercar o alejar la vista", false, "accessibility.zoom",
                  L"Al activar esta opción, puedes utilizar la función que permite acercar y alejar la imagen "
                  L"si pulsas rápidamente " G_HOME L" dos veces seguidas."),
                V(L"Cambiar colores del menú", L"Estándar", "accessibility.menu_colours"),
                // gap
                T(L"Sonido monoaural", false, "accessibility.mono"),
                // gap
                H(L"Síntesis de voz", "accessibility.tts_heading"),
                T(L"Utilizar la síntesis de voz", false, "accessibility.tts",
                  L"Se leerá en voz alta el texto que se muestra en pantalla.\n"
                  L"Mantén pulsada una palanca para escuchar de nuevo el texto leído.\n"
                  L"◆ Esta función podría no estar disponible en algunas pantallas."),
                V(L"Tipo de voz", L"Tipo 1", "accessibility.tts_voice"),
                S(L"Velocidad de la síntesis de voz", "accessibility.tts_speed"),    // fast-forward icon; knob at about 15 %
                S(L"Volumen de la síntesis de voz", "accessibility.tts_volume"),     // speaker icon; knob at about 20 %
                // gap
                A(L"GameChat: conversión voz ⇔ texto", "accessibility.gamechat_speech_text"),
            }, true},

            Section{L"Gestión de datos", "data", {
                P(L"Memoria de la consola", "data.system_storage",
                  L"Barra de uso por colores; debajo, a la izquierda «Espacio libre» y a la derecha la "
                  L"cantidad (en la captura: 59,4 GB)."),
                P(L"Tarjeta microSD Express", "data.sd_storage",
                  L"Segunda barra a la derecha, atenuada y vacía; debajo: «No se detecta ninguna.»"),
                X(L"Programas", "data.legend_software", L"Leyenda: cuadro azul."),
                X(L"Datos de guardado", "data.legend_saves", L"Leyenda: cuadro azul claro."),
                X(L"Imagen/vídeo", "data.legend_media", L"Leyenda: cuadro rojo."),
                X(L"Otros", "data.legend_other", L"Leyenda: cuadro gris."),
                H(L"Datos de programas", "data.software_data_heading"),
                A(L"Mover datos entre la consola y una tarjeta microSD Express", "data.move"),
                A(L"Borrado rápido", "data.quick_archive"),
                A(L"Gestionar programas", "data.manage_software"),
                // gap
                H(L"Programas", "data.software_heading"),
                A(L"Guardado de datos en la nube", "data.cloud_saves"),
                A(L"Transferir datos de guardado", "data.transfer_saves"),
                A(L"Borrar datos de guardado", "data.delete_saves"),
                // gap
                H(L"Imagen/vídeo", "data.media_heading"),
                A(L"Gestionar capturas y vídeos", "data.manage_media"),
            }, true},

            Section{L"Perfiles de usuario", "users", {
                P(kPersonal, "users.grid",
                  L"Cuadrícula de fichas de usuario, cuatro por fila: icono redondo y apodo debajo "
                  L"(siete usuarios en la captura; los apodos no se copian)."),
                A(L"Cambiar el orden", "users.reorder"),
                A(L"Registrar un usuario", "users.add"),
            }, true},

            Section{L"Mandos y accesorios", "controllers", {
                A(L"Cambiar el orden o modo de sujeción de los mandos", "controllers.grip_order"),
                A(L"Ajustes de los botones GL y GR", "controllers.gl_gr",
                  L"Puedes configurar como desees la asignación de los botones GL y GR de un mando Pro de "
                  L"Switch 2 o de un soporte de carga para mandos Joy-Con 2."),
                T(L"Vibración de los mandos", true, "controllers.rumble",
                  L"La función de vibración se habilitará en mandos compatibles. Para configurar la vibración "
                  L"de las alarmas, elige \"Notificaciones\" y luego selecciona \"Ajustes de alarmas\"."),
                A(L"Actualizar los mandos", "controllers.update",
                  L"Actualiza los mandos con los que está sincronizada la consola.\n"
                  L"Ten en cuenta que algunos mandos no se pueden actualizar."),
                T(L"Modo ratón", true, "controllers.mouse_mode",
                  L"Para utilizar el modo ratón, sujeta el mando Joy-Con 2 con la parte del botón SYNC "
                  L"mirando hacia abajo y colócalo sobre una superficie plana.\n"
                  L"◆ Esta función no se aplica al manejo en modo ratón dentro de los programas."),
                V(L"Sensibilidad del ratón", L"Normal", "controllers.mouse_sensitivity",
                  L"Es posible que en algunos programas la sensibilidad del ratón no varíe, "
                  L"independientemente de la configuración establecida."),
                A(L"Actualizar la cámara de Switch 2", "controllers.update_camera"),
                A(L"Probar la cámara USB", "controllers.test_camera"),
                V(L"Teclado USB", L"Español (España)", "controllers.usb_keyboard"),
                // gap
                A(L"Calibrar las palancas", "controllers.calibrate_sticks"),
                A(L"Calibrar los sensores de movimiento", "controllers.calibrate_motion"),
                A(L"Probar los dispositivos de entrada", "controllers.test_input",
                  L"Verifica si los mandos y la pantalla táctil funcionan correctamente."),
                T(L"Conexión por cable del mando Pro", false, "controllers.pro_wired",
                  L"Si activas esta opción, el mando Pro se comunicará por cable con la consola cuando esté "
                  L"conectado a ella mediante el cable USB de carga. El punto de contacto NFC del mando Pro "
                  L"dejará de funcionar mientras el mando esté conectado por cable."),
                A(L"Intercambiar " G_A L" y " G_B, "controllers.swap_ab",
                  L"Puedes intercambiar la funcionalidad de " G_A L" y " G_B L" en cada mando que utilices.\n"
                  L"Esta opción no está disponible para mandos que carecen de " G_A L" y " G_B L"."),
                A(L"Desincronizar los mandos", "controllers.disconnect"),
            }, true},

            Section{L"Sonido", "sound", {
                P(L"Salida y entrada de audio actuales", "sound.current",
                  L"Panel con dos entradas: icono de altavoz + «Televisor» e icono de micrófono + "
                  L"«Micrófono (consola)»."),
                H(L"Salida de audio", "sound.output_heading"),
                V(L"Salida de audio prioritaria", L"Automática", "sound.output_priority"),
                V(L"Sonido del televisor", L"Automático", "sound.tv_sound"),
                T(L"Sonido envolvente (altavoces de la consola)", true, "sound.surround_speakers",
                  L"Al activar esta opción, se aplicará un efecto de sonido envolvente al usar programas "
                  L"compatibles con el canal de audio 5.1."),
                T(L"Sonido envolvente (auriculares)", true, "sound.surround_headphones",
                  L"Al activar esta opción, si hay auriculares conectados, se aplicará un efecto de sonido "
                  L"envolvente al usar programas compatibles con el canal de audio 5.1."),
                T(L"Silenciar cuando se desconecten los auriculares", true, "sound.mute_on_unplug"),
                T(L"Bajar el volumen máximo de los auriculares", true, "sound.limit_headphones"),
                V(L"Modo de audio del mando Pro de Switch 2", L"Estable", "sound.pro_audio_mode",
                  L"Puedes establecer el modo de entrada/salida que se usará al conectar auriculares a un "
                  L"mando Pro de Switch 2 inalámbrico. Si seleccionas \"Baja latencia\", se reducirá la "
                  L"latencia de audio en comparación con la opción \"Estable\". Sin embargo, el sonido "
                  L"podría entrecortarse dependiendo de tu entorno de red."),
                A(L"Prueba de salida de audio", "sound.test_output"),
                // gap
                H(L"Entrada de audio", "sound.input_heading"),
                V(L"Entrada de audio del micrófono", L"Automática", "sound.mic_input"),
                T(L"Cancelar el ruido del micrófono", true, "sound.mic_noise"),
                S(L"Volumen de entrada del micrófono", "sound.mic_volume"),          // microphone icon; knob at 100 %
                A(L"Prueba de micrófono", "sound.test_mic"),
            }, true},

            Section{L"Pantallas", "display", {
                H(L"Consola", "display.console_heading"),
                T(L"Salida de HDR (consola)", true, "display.hdr_console",
                  L"El alto rango dinámico (HDR) se habilitará para todos los programas."),
                H(L"Televisor", "display.tv_heading"),
                V(L"Resolución del televisor", L"1080p (FHD)", "display.tv_resolution"),
                A(L"Datos de salida de vídeo", "display.video_output_info",
                  L"Permite consultar información sobre la señal de salida de vídeo de la consola, así como "
                  L"de las compatibles con el televisor al que esté conectada."),
                V(L"Rango RGB", L"Completo", "display.rgb_range"),
                A(L"Ajustar el tamaño de la imagen en pantalla", "display.screen_size"),
                T(L"Protector de pantalla", true, "display.burn_in",
                  L"Si se tiene activada esta opción, el brillo de la pantalla se reducirá tras cinco minutos "
                  L"de inactividad."),
                T(L"Sincronizar con el televisor", true, "display.tv_power_sync",
                  L"El televisor se encenderá tanto si la consola pasa al modo TV como si se desactiva el modo "
                  L"de espera con la consola en modo TV. Al apagar el televisor, se activará el modo de espera.\n"
                  L"◆ Esta función no es compatible con ciertos modelos. Comprueba los ajustes de HDMI en el televisor."),
                T(L"Salida de 120 Hz", true, "display.120hz",
                  L"Se aplicará una frecuencia de salida de 120 Hz cuando sea compatible con el televisor y el "
                  L"programa utilizados.\n"
                  L"◆ Las resoluciones del televisor compatibles son 1080p y 1440p."),
                T(L"Modo automático de baja latencia (ALLM)", true, "display.allm",
                  L"Se activará el modo automático de baja latencia en televisores compatibles."),
                A(L"Ajustes de VRR", "display.vrr"),
                V(L"Salida de HDR", L"Siempre activada", "display.hdr_output",
                  L"Si se selecciona la opción \"Siempre activada\", el alto rango dinámico (HDR) se habilitará "
                  L"con independencia del programa usado. En cambio, con la opción \"En programas compatibles\", "
                  L"únicamente se habilitará para los programas compatibles con HDR.\n"
                  L"◆ También será necesario configurar el alto rango dinámico en tu televisor."),
                A(L"Ajustes del HDR", "display.hdr_settings"),
            }, true},

            Section{L"Mii", "mii", {
                X(L"Puedes crear tus propios Mii combinando distintos rasgos faciales y corporales, y luego "
                  L"utilizarlos en programas compatibles.", "mii.intro"),            // Mii logo on the left
                A(L"Lista de Mii", "mii.list"),
                A(L"Crear un nuevo Mii", "mii.create"),
                // gap
                A(L"Código QR", "mii.qr",
                  L"Es posible convertir personajes Mii en códigos QR para intercambiarlos con otras personas."),
                A(L"Enviar y recibir Mii", "mii.send_receive",
                  L"Puedes establecer una conexión con otra consola para enviar o recibir personajes Mii."),
            }, true},

            Section{L"amiibo", "amiibo", {
                X(L"Aquí puedes editar y borrar los datos guardados en los accesorios amiibo.", "amiibo.intro"),   // amiibo logo on the left
                A(L"Registrar apodo y propietario", "amiibo.register"),
                A(L"Borrar datos de juego de un amiibo", "amiibo.delete_data"),
                A(L"Formatear un amiibo", "amiibo.reset"),
            }, true},

            // A radio list: each row has a colour swatch on the left and a radio button on the right.
            // "on" is the selected one (dark theme during most of the capture; switched to white at 274 s).
            Section{L"Temas", "themes", {
                T(L"Blanco sencillo", false, "themes.basic_white"),
                T(L"Negro sencillo", true, "themes.basic_black"),
            }, true},

            Section{L"Notificaciones", "notifications", {
                T(L"Notificaciones de descargas finalizadas", true, "notifications.downloads"),
                A(L"Ajustes de las notificaciones entre amigos", "notifications.friends"),
                A(L"Ajustes de las notificaciones de Switch Online", "notifications.nso"),
                A(L"Ajustes de alarmas", "notifications.alarms",
                  L"Si estableces una alarma en un programa compatible, recibirás una notificación en el día "
                  L"y hora indicados."),
                T(L"Alarma de notificación", true, "notifications.alarm_sound"),
            }, true},

            // The note under the two "automático" rows belongs to both.
            Section{L"Modo de espera", "sleep", {
                V(L"Modo de espera automático (consola)", L"10 min", "sleep.auto_console"),
                V(L"Modo de espera automático (televisor)", L"15 min", "sleep.auto_tv",
                  L"Si aumentas el intervalo de tiempo o seleccionas \"Nunca\", la consola consumirá más "
                  L"energía. Cuando esté activado el modo de espera automático, puede que la consola tarde "
                  L"unos minutos en alcanzar el estado de consumo de energía más bajo."),
                T(L"Modo de espera automático (reproducir contenido)", true, "sleep.auto_media",
                  L"Aunque tengas activada esta opción, el modo de espera automático solo se activará al "
                  L"alcanzar cuatro horas de reproducción."),
                V(L"Sonido al salir del modo de espera (TV)", L"Normal", "sleep.wake_sound"),
                T(L"Mantener la conexión con cable en modo de espera", false, "sleep.wired_connection",
                  L"Al activar esta opción, si la consola está conectada con cable a internet, las descargas "
                  L"comenzarán inmediatamente incluso en modo de espera, pero la consola consumirá más energía."),
                T(L"Salir si se desconecta el adaptador", true, "sleep.wake_on_unplug",
                  L"Al activar esta opción, la consola también saldrá del modo de espera si se retira de la base."),
            }, true},

            Section{L"Consola", "system", {
                A(L"Actualizar la consola", "system.update", L"Versión actual: 23.5.0"),
                A(L"Actualizar la base", "system.update_dock",
                  L"Actualiza la versión de firmware de la base de Switch 2."),
                V(L"Apodo de la consola", kPersonal, "system.nickname"),
                // gap
                T(L"Bloquear la pantalla en modo de espera", true, "system.screen_lock",
                  L"Al salir del modo de espera, pulsa el mismo botón tres veces para continuar.\n"
                  L"(Esta función está desactivada si la consola está conectada a un televisor.)"),
                T(L"Bloquear la consola", false, "system.console_lock",
                  L"Establece una contraseña de modo que solo quienes la conozcan puedan utilizar la consola. "
                  L"Será necesario introducir la contraseña cuando la consola salga del modo de espera."),
                V(L"Idioma", L"Español (España)", "system.language"),
                V(L"Región", L"Europa", "system.region"),
                A(L"Fecha y hora", "system.date_time",
                  L"Fecha y hora actuales: 26-11-2026 04:42"),                      // live clock: dd-mm-yyyy hh:mm
                T(L"Batería de la consola (%)", true, "system.battery_percent"),
                T(L"Detener la carga de la batería al 80-90 %", false, "system.charge_limit",
                  L"Al activar esta opción, la consola dejará de cargarse antes con el fin de optimizar la "
                  L"vida útil de la batería.\n"
                  L"◆ Aun con esta opción activada, es posible que en ocasiones la consola se cargue al 100 % "
                  L"para que el nivel de batería se muestre correctamente."),
                A(L"Tratamiento de programas de Switch", "system.switch1_software"),
                // gap
                A(L"Números de serie", "system.serials"),
                A(L"Información importante", "system.important_info"),
                A(L"Avisos sobre la propiedad intelectual", "system.ip_notices"),
                A(L"Información adicional", "system.additional_info"),
                // gap
                T(L"Actualización automática de programas", false, "system.auto_update"),
                T(L"Compartir información de errores", false, "system.share_errors"),
                // gap
                A(L"Transferir a una consola Switch 2", "system.transfer"),
                // gap
                A(L"Opciones de formateo", "system.format"),
            }, true},
        }};
    return m;
}

// ---------------------------------------------------------------------------------------------
// Página de <apodo>  (title bar: round user icon + "Página de <apodo>")
// Left list: thin separators after "Perfil" and after "Añadir un amigo". The entry
// "Lista de amigos" carries a grey second line "En línea: 0".
// Footer: (B) Atrás  (A) Confirmar; see the sections for the extra hints.
// ---------------------------------------------------------------------------------------------
const Menu& UserPage() {
    static const Menu m{
        L"Página de —",
        {
            Section{L"Perfil", "profile", {
                // One focusable card: large round icon, nickname, grey status line under it and a
                // pencil icon on the right. Status in the capture: "Me desconecté hace 12 días."
                V(kPersonal, kNone, "profile.card", L"Me desconecté hace 12 días."),
                V(L"Clave de amigo:", kPersonal, "profile.friend_code"),            // format SW-0000-0000-0000
                H(L"Historial de programas usados", "profile.history_heading"),      // round "?" button at the right end
                // One row per program (icon, title, play time line); 20-odd rows in the capture, not copied.
                // Play time wordings seen: "He jugado 85 horas o más.", "He jugado 1 hora o más.",
                // "He jugado un rato." (blue for the most played, grey otherwise).
                V(kPersonal, kNone, "profile.history_item", L"He jugado 85 horas o más."),
            }, true},

            // Shows a spinner, then this sentence centred. Footer adds:
            // (+) ¿No puedes encontrar una sala de chat?   (Y) Actualizar
            Section{L"Invitaciones", "invitations", {
                X(L"Las invitaciones para usar GameChat o jugar en línea aparecerán aquí.", "invitations.empty"),
            }, true},

            // Footer adds: (Y) Actualizar
            Section{L"Lista de amigos", "friends", {
                P(kPersonal, "friends.grid",
                  L"Cuadrícula de fichas de amigo, cuatro por fila: estrella azul arriba a la izquierda en "
                  L"los mejores amigos, icono redondo, apodo y una línea de estado en gris "
                  L"(«Hace 23 horas», «Sin conexión»). Los apodos no se copian."),
            }, true},

            Section{L"Añadir un amigo", "add_friend", {
                A(L"Solicitudes de amistad recibidas", "add_friend.received"),
                // gap
                A(L"Buscar por clave de amigo", "add_friend.by_code"),
                A(L"Buscar usuarios con los que has jugado", "add_friend.played_with"),
                A(L"Buscar mediante la comunicación local", "add_friend.local"),
                A(L"Solicitudes de amistad enviadas", "add_friend.sent"),
                V(L"Tu clave de amigo:", kPersonal, "add_friend.own_code"),          // small grey line, right-aligned
            }, true},

            Section{L"Ajustes de usuario", "user_settings", {
                H(L"Ajustes del perfil", "user_settings.profile_heading"),
                V(L"Icono", kNone, "user_settings.icon"),                           // the value is the round user icon
                V(L"Apodo", kPersonal, "user_settings.nickname"),
                A(L"Ajustes del historial de programas usados", "user_settings.history"),
                // The heading shows the masked account name in brackets, e.g. "Cuenta Nintendo (abc•••)".
                H(L"Cuenta Nintendo (—)", "user_settings.account_heading"),
                A(L"Consultar los ajustes de la cuenta", "user_settings.account"),
                A(L"Mostrar código QR para registrarse", "user_settings.signin_qr"),
                A(L"Ajustes de verificación de identidad", "user_settings.identity",
                  L"Puedes establecer que el uso de ciertas funciones requiera verificación."),
                A(L"Ver consolas que pueden usar tarjetas de juego virtuales", "user_settings.vcard_consoles"),
                A(L"Ajustes de las licencias en línea", "user_settings.online_licences"),
                H(L"Switch Online", "user_settings.nso_heading"),
                A(L"Ver el estado de la suscripción en Nintendo eShop", "user_settings.nso_membership"),
                A(L"Ajustes de las notificaciones de Switch Online", "user_settings.nso_notifications"),
                H(L"Funciones de amigos", "user_settings.friends_heading"),
                A(L"Ajustes de amigos", "user_settings.friend_settings"),
                A(L"Comprobar lista de usuarios bloqueados", "user_settings.blocked"),
                H(L"GameChat", "user_settings.gamechat_heading"),
                X(L"Para acceder a los ajustes de GameChat, pulsa " G_C L" en el mando Joy-Con 2 derecho y "
                  L"selecciona \"Ajustes\".", "user_settings.gamechat_text",
                  L"◆ También puedes hacerlo si abres GameChat (" G_C L") desde el menú HOME."),
                H(L"Álbum", "user_settings.album_heading"),
                A(L"Ajustes de subida de archivos", "user_settings.album_upload"),
                H(L"Nintendo eShop", "user_settings.eshop_heading"),
                A(L"Ajustes de Nintendo eShop", "user_settings.eshop",
                  L"Aquí puedes consultar información relativa a los métodos de pago y las reservas, entre "
                  L"otras cosas."),
                A(L"Actualizar progreso de la descarga", "user_settings.refresh_downloads"),
                H(L"Noticias", "user_settings.news_heading"),
                A(L"Ajustes de los canales", "user_settings.news_channels"),
                H(L"Ayuda al usuario", "user_settings.support_heading"),
                A(L"Código de ayuda", "user_settings.support_code"),
            }, true},
        }};
    return m;
}

// ---------------------------------------------------------------------------------------------
// Game options (+ on a game). Header: game icon, title, and under it "Versión: 1.1.1  |  Nintendo"
// (version and publisher of that game). The first and last left entries are written on two lines
// on the console: "Información / del programa", "Guardado de datos / en la nube".
// Footer: (B) Atrás  (A) Confirmar.
// ---------------------------------------------------------------------------------------------
const Menu& GameOptions() {
    static const Menu m{
        L"—",   // the game's title
        {
            Section{L"Información del programa", "info", {
                V(L"Tipo de programa", L"Programa de Switch 2", "info.type"),   // a panel: label above, value below, indented
                // gap
                A(L"Nintendo eShop", "info.eshop"),
                A(L"Tarjeta de juego virtual", "info.virtual_card"),
                // gap
                A(L"Información de ayuda", "info.support"),
                A(L"Información importante", "info.important"),
                A(L"Avisos sobre la propiedad intelectual", "info.ip_notices"),
            }, true},

            Section{L"Actualizar", "update", {
                A(L"A través de internet", "update.internet",
                  L"Si todavía no se puede utilizar el contenido descargable o el paquete de mejora tras "
                  L"haber actualizado el programa, vuelve a cargar la tarjeta de juego virtual de dicho "
                  L"programa desde la sección de tarjetas de juego virtuales (" G_VCARD L") del menú HOME."),
                A(L"Igualar versiones por comunicación local", "update.local",
                  L"Puedes usar la comunicación local para igualar tu versión de un programa con la de "
                  L"otros usuarios."),
            }, true},

            Section{L"Gestión de datos", "data", {
                A(L"Transferir datos de guardado", "data.transfer_saves",
                  L"Conéctate a una consola que esté cerca para transferir datos de guardado."),
                A(L"Borrar el programa", "data.delete_software",
                  L"Se conservarán los datos de guardado. Cuando quieras volver a usar el programa, tendrás "
                  L"que descargarlo de nuevo."),
            }, true},

            // One row per console user: round icon, nickname, status line under it. Users without
            // save data are greyed out. Not scrolled: the last users were off screen. Nicknames are
            // not copied; the three rows below are the three status wordings that were visible.
            Section{L"Guardado de datos en la nube", "cloud", {
                X(L"Aquí se muestra el estado actual del guardado rápido de los datos de cada usuario.",
                  "cloud.intro"),                                                   // round "?" button at the right end
                V(kPersonal, kNone, "cloud.user_not_backed_up", L"Guardado rápido no efectuado"),
                V(kPersonal, kNone, "cloud.user_no_data", L"Sin datos de guardado"),
                V(kPersonal, kNone, "cloud.user_unknown", L"No se ha podido comprobar el estado."),
            }, false},
        }};
    return m;
}

}  // namespace console
