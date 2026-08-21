#pragma once

#include <stdint.h>

// The other side of the glass: what it is running on and where it is, rather
// than what it is feeling. A charge, a network and the commit it was built from.
//
// Nothing here moves, so it is drawn only when something it says has changed -
// which is the battery every few seconds and nothing else - and drawn whole
// when it is, because a page this still is not worth the bookkeeping that
// partial redraws cost everywhere else.
void infoStep(uint16_t *fb);

// Forgets what it last put down, so the next step draws all of it. For arriving
// at the page, which is the one time nothing has changed and all of it still
// has to go back on the glass.
void infoForget();
