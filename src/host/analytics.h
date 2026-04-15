/*
 * Anonymous PostHog Analytics - fire-and-forget HTTP POSTs
 *
 * Disabled by default. Enabled when /data/UserData/schwung/analytics-opt-in exists.
 * Anonymous UUID stored in /data/UserData/schwung/anonymous-id.
 */

#ifndef ANALYTICS_H
#define ANALYTICS_H

/* Initialize analytics: load or generate anonymous UUID */
void analytics_init(const char *version);

/* Track an event (fire-and-forget, no-op if disabled) */
void analytics_track(const char *event, const char *properties_json);

/* Check if analytics is enabled */
int analytics_enabled(void);

/* Enable/disable analytics (creates/removes opt-in file) */
void analytics_set_enabled(int enabled);

#endif /* ANALYTICS_H */
