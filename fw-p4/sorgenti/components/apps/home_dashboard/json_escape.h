#pragma once
#include <string>

// Mette un testo dentro una stringa JSON, virgolette comprese.
//
// Serve tutte le volte che si costruisce un messaggio per Home Assistant a
// mano invece che con cJSON: un apostrofo tipografico, una barra o un a capo
// dentro il testo romperebbero il messaggio, e HA lo scarterebbe senza dire
// perche'. Lo usano la card markdown (il modello Jinja ci finisce dentro
// tale e quale) e l'annuncio del pannello all'integrazione.
std::string json_escape(const char *in);
