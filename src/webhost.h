#pragma once

void webInit();
void webHandle();
void setDatabaseEnabled(bool enabled);

String loadCardForCurrentID();
void queueCardUpload();
void setCurrentID(const String& id);
