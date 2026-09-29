"""Nomi condivisi fra i file dell'integrazione."""

DOMAIN = "crowpanel"

# Evento del bus con cui Home Assistant comanda i pannelli. E' un evento
# normale, visibile in Strumenti per sviluppatori > Eventi: cosi' si puo'
# provare un comando a mano prima di metterlo in un'automazione.
EVENT_COMMAND = "crowpanel_command"

# Segnali interni verso le entita'
SIGNAL_UPDATE = "crowpanel_update"          # + "_" + id del pannello
SIGNAL_NEW_PANEL = "crowpanel_new_panel"

# Quanto silenzio prima di dare il pannello per irraggiungibile. Lo stato
# arriva ogni 30 s: tre giri persi vogliono dire che qualcosa non va.
OFFLINE_AFTER_S = 100

CONF_PANELS = "pannelli"
CONF_MANAGED = "gestito_da_ha"
CONF_VALUES = "valori"
CONF_AFTER_MIN = "slideshow_dopo_min"
CONF_OFF_MIN = "spegni_dopo_min"
CONF_PHOTO_S = "secondi_foto"
CONF_SHUFFLE = "casuale"
CONF_FOLDER = "cartella"
