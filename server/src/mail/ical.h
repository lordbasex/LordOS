/* ical.h — iCalendar (RFC 5545) and the shared content-line syntax of vCard (RFC 6350) for
 * axia-maild: calendars, reminders (VTODO) and invitations (iTIP, RFC 5546).
 *
 * The parser is bounded (line count, nesting, property count) and never reads outside the input.
 * Times are kept as Unix seconds (UTC). An all-day value keeps its civil date (YYYYMMDD) so it
 * never shifts with the time zone. TZID values are resolved with the system tz database
 * (/usr/share/zoneinfo, through our TZif reader); non-IANA zone names (as some calendar servers send) are mapped. */
#ifndef LORD_MAIL_ICAL_H
#define LORD_MAIL_ICAL_H

#include <stddef.h>
#include <stdint.h>
#include "../doc/wire.h"

#define IC_MAX_LINES  200000
#define IC_MAX_DEPTH  8
#define IC_MAX_PROPS  4000

typedef struct {
    char name[48];          /* upper case, without the vCard group ("item1.") */
    char group[24];
    char *params;           /* raw "TZID=Europe/Madrid;VALUE=DATE" (may be "") */
    char *value;            /* raw value (still escaped) */
} IcProp;

typedef struct IcComp IcComp;
struct IcComp {
    char name[24];          /* "VCALENDAR", "VEVENT", "VCARD", ... ("" for the root) */
    IcProp *props;
    int nprops, capprops;
    IcComp *kid, *last, *next, *parent;
};

/* Content lines → component tree. NULL on error (err set). */
IcComp *ic_parse(const char *text, size_t len, const char **err);
void    ic_free(IcComp *root);
IcProp *ic_prop(const IcComp *c, const char *name);                    /* first one */
IcProp *ic_prop_next(const IcComp *c, const IcProp *after, const char *name);
int     ic_param(const IcProp *p, const char *name, char *out, size_t cap);   /* 1 if present */
/* TEXT value unescaping (\n \N \, \; \\) into out. */
void    ic_unescape(const char *in, char *out, size_t cap);
char   *ic_unescape_dup(const char *in);
/* Escapes text for a TEXT value. malloc. */
char   *ic_escape(const char *in);
/* Folds a content line at 75 octets (UTF-8 safe) and appends it with CRLF. */
void    ic_line(JBuf *b, const char *line);

/* ---- time ---- */
/* Sets the user's zone (IANA name, e.g. "America/Argentina/Buenos_Aires") for floating times. */
void    ic_set_local_zone(const char *zone);
const char *ic_local_zone(void);
/* Offset (seconds east of UTC) of a zone at instant t; "" or unknown zone = the local zone. */
int32_t ic_zone_offset(const char *zone, int64_t t);
/* Wall-clock seconds (as if UTC) in `zone` → Unix seconds. */
int64_t ic_wall_to_utc(int64_t wall, const char *zone);
/* Parses DATE / DATE-TIME. *allday = 1 for VALUE=DATE (then *t is that date's 00:00 local). */
int     ic_time(const IcProp *p, int64_t *t, int *allday, char *tzid, size_t tzcap);
/* civil date helpers */
int64_t ic_days_from_civil(int64_t y, int m, int d);
void    ic_civil_from_days(int64_t z, int *y, int *m, int *d);

/* ---- events and to-dos ---- */
#define IC_MAX_ATT 64
typedef struct { char email[256]; char name[160]; char partstat[24]; char role[24]; int rsvp; } IcAttendee;

typedef struct {
    int is_todo;
    char uid[256];
    char summary[512];
    char location[512];
    char *description;          /* malloc, unescaped */
    char url[512];
    char status[24];            /* CONFIRMED, TENTATIVE, CANCELLED, NEEDS-ACTION, COMPLETED */
    char categories[256];
    int64_t start, end;         /* UTC; end = start (+1 day for all-day) if missing */
    int allday;
    int sdate, edate;           /* all-day: civil days (ic_days_from_civil) */
    char tzid[64];
    char rrule[512];
    int64_t exdate[64];
    int nexdate;
    int64_t recurrence_id;      /* override of one occurrence (0 if none) */
    int alarms[8];              /* minutes before start (or due) */
    int nalarms;
    char organizer[256], organizer_name[160];
    IcAttendee *att;            /* malloc, natt entries (at most IC_MAX_ATT) */
    int natt;
    int sequence;
    int64_t dtstamp, modified;
    /* VTODO */
    int64_t due;
    int due_allday;
    int priority;               /* 0 none, 1 high … 9 low */
    int64_t completed;          /* 0 = not completed */
    int flagged;                /* X-LORDOS-FLAGGED / X-APPLE-FLAGGED? (local extension) */
    char parent_uid[256];       /* RELATED-TO (subtasks) */
} IcEvent;

/* All VEVENT/VTODO of a calendar text. *method gets METHOD (REQUEST, REPLY, CANCEL…) or "".
 * Returns the count (malloc'd array in *out) or -1. */
int  ical_events(const char *text, size_t len, IcEvent **out, char *method, size_t mcap);
void ical_events_free(IcEvent *ev, int n);
/* Occurrences of ev that intersect [from, to): calls fn with each start (UTC). Overrides and
 * EXDATE are the caller's job for overrides; EXDATE is applied here. Returns the count, bounded
 * by `max`. */
int  ical_expand(const IcEvent *ev, int64_t from, int64_t to, int max, void (*fn)(void *u, int64_t start), void *u);
/* JSON of one event/to-do (occurrence start overrides when occ != 0). */
void ical_event_json(JBuf *b, const IcEvent *ev, int64_t occ);

/* Builds a VCALENDAR with one VEVENT or VTODO from JSON-like fields. The caller frees. */
typedef struct {
    int is_todo;
    const char *uid, *summary, *location, *description, *url, *rrule, *status, *categories;
    int64_t start, end; int allday; const char *tzid;
    const int *alarms; int nalarms;
    int64_t due; int due_allday; int priority; int64_t completed; int flagged;
    const char *organizer, *organizer_name;
    const IcAttendee *att; int natt;
    int sequence;
    const char *method;         /* NULL for a plain calendar object */
    const char *parent_uid;
    const int64_t *exdate; int nexdate;
} IcBuild;
char *ical_build(const IcBuild *b, size_t *olen);
/* iTIP REPLY for an invitation: the original event with only `attendee`'s PARTSTAT. */
char *ical_reply(const IcEvent *ev, const char *attendee, const char *name, const char *partstat, size_t *olen);

#endif
