#!/bin/sh
# wpewebkit-chroot.sh — the part of tools/pkg/build-wpewebkit.sh that runs INSIDE a clean Debian 13 chroot made by
# mmdebstrap (docs/WPEWEBKIT.md). Never run it on a real system: it installs packages and adds a user.
#
#   build <debian-version> <lordos-version>   verify Debian's source, make the LordOS backport, build it
#                                            (source + every binary), lintian; reads /build/in, writes /build/out
#   test                                     install /build/debs/*.deb, check pkg-config, the ABI against
#                                            trixie's 2.48 and render a page with WPEPlatform's headless backend
#
# The changes to Debian's packaging are all here, in backport(), so a new WebKit release needs no edit unless one
# of them stops applying (then the build stops and says which).
set -eu
export LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

die() { echo "wpewebkit-chroot: $*" >&2; exit 1; }

# The keys that sign WebKitGTK and WPE WebKit releases (https://wpewebkit.org/release/verify/). Debian ships them in
# debian/upstream/signing-key.asc; a signature by any other key in that file is refused.
UPSTREAM_FPRS="5AA3BC334FD7E3369E7C77B291C559DBE4C9123B 013A0127AC9C65B34FFA62526C1009B693975393"
DEBEMAIL_LORDOS="LordOS <lord.basex@gmail.com>"

# the .dsc is signed by a Debian Developer (debian-keyring) and every file matches its checksums; the upstream
# tarball's own signature verifies with one of the pinned release keys, and wpewebkit.org's copy of that signature is
# the same file (so Debian's .orig is the very tarball WebKit released)
verify_source() {
    dsc=$1 up=$2
    dscverify --keyring /usr/share/keyrings/debian-keyring.gpg "$dsc" || die "$dsc: the Debian signature or a checksum does not verify"
    t=$(mktemp -d)
    dpkg-source --no-check -x "$dsc" "$t/src" >/dev/null
    gpg --batch --quiet --homedir "$t" --dearmor -o "$t/upstream.gpg" "$t/src/debian/upstream/signing-key.asc"
    orig=/build/in/wpewebkit_$up.orig.tar.xz
    [ -f "$orig.asc" ] || die "no $orig.asc next to the .orig tarball"
    gpgv --status-fd 3 --keyring "$t/upstream.gpg" "$orig.asc" "$orig" 3> "$t/status" >/dev/null 2>&1 || die "$orig: the upstream signature does not verify"
    fpr=$(awk '$2 == "VALIDSIG" { print $3 }' "$t/status")
    case " $UPSTREAM_FPRS " in *" $fpr "*) ;; *) die "$orig is signed by $fpr, not by a pinned WebKit release key" ;; esac
    echo "upstream signature: good, key $fpr"
    if [ -f "/build/in/wpewebkit-$up.tar.xz.asc" ]; then
        cmp -s "/build/in/wpewebkit-$up.tar.xz.asc" "$orig.asc" || die "wpewebkit.org's signature differs from Debian's .orig.tar.xz.asc"
        echo "wpewebkit.org's signature is the same file as Debian's"
    fi
    rm -rf "$t"
}

# the LordOS changes to Debian's packaging
backport() {
    newver=$1
    # 1. WPEPlatform (wpe-platform-2.0 with its headless and Wayland backends) and the legacy libwpe API both on,
    #    explicitly: they are upstream's defaults today, and a default that changes must stop the build, not ship.
    #    WPEPlatform's DRM backend off (maintainer's decision, 2026-11-24): it is the only part that links libinput and
    #    libudev, which LordOS's policy keeps out (ARCHITECTURE.md), and lord-web never drives DRM itself
    grep -q '^	   -DUSE_LIBBACKTRACE=OFF$' debian/rules || die "debian/rules changed: the cmake flags anchor is gone"
    sed -i 's/^	   -DUSE_LIBBACKTRACE=OFF$/	   -DUSE_LIBBACKTRACE=OFF \\\
	   -DENABLE_WPE_PLATFORM=ON \\\
	   -DENABLE_WPE_PLATFORM_DRM=OFF \\\
	   -DENABLE_WPE_PLATFORM_HEADLESS=ON \\\
	   -DENABLE_WPE_PLATFORM_WAYLAND=ON \\\
	   -DENABLE_WPE_LEGACY_API=ON \\\
	   -DENABLE_MEDIA_STREAM=ON \\\
	   -DENABLE_WEB_RTC=ON \\\
	   -DUSE_GSTREAMER_WEBRTC=ON \\\
	   -DUSE_LIBRICE=ON/' debian/rules
    #    WebRTC (maintainer's decision, 2026-11-25: voice and video calls in web apps): WebKit's GStreamer backend
    #    (webrtcbin, GStreamer 1.26's), not Google's libwebrtc, and getUserMedia (MediaStream, on by default, written
    #    out). Its only new build dependency is OpenSSL 3 (libssl-dev: WebCore links libcrypto for SFrame); webrtcbin's
    #    own pieces come at run time: libnice's ICE (gstreamer1.0-nice), DTLS, SRTP and SCTP (plugins-bad), RTP,
    #    VP8 and Opus (plugins-good and -base), H.264 through OpenH264 (plugins-bad). ICE with librice (+4, maintainer's
    #    decision of 2026-11-25): WebKit then runs ICE and its UDP and TCP sockets in its network process, which has the
    #    network already, and the web process keeps none (bubblewrap's --unshare-net, unchanged). librice isn't in
    #    Debian 13: LordOS builds it (tools/pkg/build-librice.sh) and this build installs it from EXTRA_DEBS
    grep -q '^               libgstreamer-plugins-bad1.0-dev,$' debian/control || die "debian/control changed: the Build-Depends anchor is gone"
    sed -i 's/^               libgstreamer-plugins-bad1.0-dev,$/               libgstreamer-plugins-bad1.0-dev,\
               libssl-dev,\
               librice-dev (>= 0.4.3),/' debian/control
    #    what WebRTC needs at run time, as Recommends of the library (like any GStreamer element WebKit loads)
    awk '/^Package: libwpewebkit-2.0-1$/ { p = 1 } p && /^Suggests:/ { print "Recommends: gstreamer1.0-nice, gstreamer1.0-plugins-bad, gstreamer1.0-plugins-good, gstreamer1.0-plugins-base"; p = 0 } { print }' \
        debian/control > debian/control.new && mv debian/control.new debian/control
    grep -q '^Recommends: gstreamer1.0-nice' debian/control || die "could not add the WebRTC Recommends to debian/control"
    #    SharedArrayBuffer only for cross-origin-isolated documents (COOP same-origin + COEP require-corp), as Safari and
    #    Chrome do: WebKit already gives such a document a web process of its own (CrossOriginMode::Isolated) but only
    #    its Cocoa launcher turns SharedArrayBuffer on there; this does the same in the GLib launcher (JSC's
    #    useSharedArrayBuffer in that process's environment, and removed from every other's). A messaging web app's
    #    calls need it (docs/WEBAPPS.md "Calls")
    cat > debian/patches/lordos-sharedarraybuffer-coi.patch <<'PATCH'
Description: SharedArrayBuffer in cross-origin-isolated web processes only
 WebProcessProxy::shouldEnableSharedArrayBuffer() is true for a web process made for a cross-origin-isolated browsing
 context group (COOP same-origin and COEP require-corp). The Cocoa process launcher turns SharedArrayBuffer on in such
 a process; the GLib one ignored it, so SharedArrayBuffer was never available. Set JSC's useSharedArrayBuffer in that
 process's environment only, and remove it from every other process's.
Author: LordOS <lord.basex@gmail.com>
Forwarded: no
Last-Update: 2026-11-25

--- a/Source/WebKit/UIProcess/Launcher/glib/ProcessLauncherGLib.cpp
+++ b/Source/WebKit/UIProcess/Launcher/glib/ProcessLauncherGLib.cpp
@@ -235,6 +235,14 @@
     int webkitClientSocketValue = webkitSocketPair.client.release();
     g_subprocess_launcher_take_fd(launcher.get(), webkitClientSocketValue, webkitClientSocketValue);
 
+    // LordOS: SharedArrayBuffer only in a web process made for cross-origin-isolated documents (COOP same-origin and
+    // COEP require-corp: WebProcessProxy's CrossOriginMode::Isolated), as WebKit's Cocoa ports do; never in any other
+    // process, whatever the embedder's environment says.
+    if (m_launchOptions.processType == ProcessLauncher::ProcessType::Web && m_client && m_client->shouldEnableSharedArrayBuffer())
+        g_subprocess_launcher_setenv(launcher.get(), "JSC_useSharedArrayBuffer", "1", TRUE);
+    else
+        g_subprocess_launcher_unsetenv(launcher.get(), "JSC_useSharedArrayBuffer");
+
 #if USE(SYSPROF_CAPTURE)
     UnixFileDescriptor sysprofFd;
 
PATCH
    echo lordos-sharedarraybuffer-coi.patch >> debian/patches/series
    #    TURN URLs with RFC 7065's transport query (turn:host:443?transport=tcp, as a video meeting site and others hand
    #    out) were refused with a SyntaxError by RTCPeerConnection's check, which is inverted: the GStreamer ICE agent
    #    reads that query, so the check now accepts exactly transport=udp and transport=tcp (+4)
    cat > debian/patches/lordos-turn-transport-query.patch <<'PATCH'
Description: accept TURN URLs with ?transport=udp|tcp in RTCPeerConnection
 RTCPeerConnection::iceServersFromConfiguration refused every TURN URL that has a query: URL::query() has no "?",
 so findIgnoringASCIICase("?transport=") is notFound, and !!notFound is true. Sites hand out
 "turn:host:443?transport=tcp" (RFC 7065), and the GStreamer ICE agent reads that parameter. Accept exactly
 transport=udp and transport=tcp; any other query is still a SyntaxError.
Author: LordOS <lord.basex@gmail.com>
Forwarded: no
Last-Update: 2026-11-26

--- a/Source/WebCore/Modules/mediastream/RTCPeerConnection.cpp
+++ b/Source/WebCore/Modules/mediastream/RTCPeerConnection.cpp
@@ -555,7 +555,9 @@
             if (!serverURL.hasOpaquePath())
                 return Exception { ExceptionCode::SyntaxError, "STUN/TURN URL should be opaque-path"_s };
             if (serverURL.protocolIs("turn"_s) || serverURL.protocolIs("turns"_s)) {
-                if (serverURL.hasQuery() && !!serverURL.query().findIgnoringASCIICase("?transport="_s))
+                // LordOS: RFC 7065's only query, "transport=udp" or "transport=tcp" (the ICE agent reads it). The check
+                // before refused every TURN URL with a query: query() has no "?", so find() was notFound, which is true.
+                if (serverURL.hasQuery() && !equalLettersIgnoringASCIICase(serverURL.query(), "transport=udp"_s) && !equalLettersIgnoringASCIICase(serverURL.query(), "transport=tcp"_s))
                     return Exception { ExceptionCode::SyntaxError, "Invalid TURN URL query string"_s };
                 if (server.credential.isNull() || server.username.isNull())
                     return Exception { ExceptionCode::InvalidAccessError, "TURN/TURNS server requires both username and credential"_s };
PATCH
    echo lordos-turn-transport-query.patch >> debian/patches/series
    #    Document-Isolation-Policy (+4.1): a messaging web app makes itself cross-origin isolated with DIP
    #    "isolate-and-require-corp" and COEP require-corp next to COOP same-origin-allow-popups, which Chrome honours and
    #    WebKit doesn't know, so it had no SharedArrayBuffer and offered no calls. Such a document (DIP isolate-and-*, an
    #    enforced COEP require-corp, COOP unsafe-none or same-origin-allow-popups) is treated as COOP same-origin + COEP:
    #    WebKit's own isolation, an isolated browsing context group in a web process of its own (docs/WPEWEBKIT.md)
    cat > debian/patches/lordos-document-isolation-policy.patch <<'PATCH'
Description: Document-Isolation-Policy makes a COEP require-corp document cross-origin isolated
 WebKit doesn't implement Document-Isolation-Policy, so a site that relies on it for cross-origin isolation (DIP
 isolate-and-require-corp with COOP same-origin-allow-popups) never gets SharedArrayBuffer. A response with DIP
 isolate-and-require-corp or isolate-and-credentialless, an enforced COEP of require-corp and COOP unsafe-none or
 same-origin-allow-popups gets the effective COOP same-origin + COEP: WebKit's own cross-origin isolation, a new
 isolated browsing context group in a web process of its own. Its COOP only becomes stricter (popups to other
 origins lose their opener); COEP and CORP enforcement of subresources is unchanged.
Author: LordOS <lord.basex@gmail.com>
Forwarded: no
Last-Update: 2026-11-26

--- a/Source/WebCore/loader/CrossOriginOpenerPolicy.cpp
+++ b/Source/WebCore/loader/CrossOriginOpenerPolicy.cpp
@@ -208,6 +208,18 @@
     return newCOOPEnforcementResult;
 }
 
+// LordOS: Document-Isolation-Policy (https://github.com/explainers-by-googlers/document-isolation-policy), which
+// WebKit doesn't implement: true when the response asks for its document to be isolated ("isolate-and-require-corp" or
+// "isolate-and-credentialless"; "none", a report-only header or anything else is not).
+static bool documentIsolationPolicyIsolates(const ResourceResponse& response)
+{
+    auto parsed = RFC8941::parseItemStructuredFieldValue(response.httpHeaderField("Document-Isolation-Policy"_s));
+    if (!parsed)
+        return false;
+    auto* token = std::get_if<RFC8941::Token>(&parsed->first);
+    return token && (token->string() == "isolate-and-require-corp"_s || token->string() == "isolate-and-credentialless"_s);
+}
+
 // https://html.spec.whatwg.org/multipage/origin.html#obtain-coop
 CrossOriginOpenerPolicy obtainCrossOriginOpenerPolicy(const ResourceResponse& response)
 {
@@ -247,6 +259,18 @@
 
     parseCOOP(HTTPHeaderName::CrossOriginOpenerPolicy, policy.value, policy.reportingEndpoint);
     parseCOOP(HTTPHeaderName::CrossOriginOpenerPolicyReportOnly, policy.reportOnlyValue, policy.reportOnlyReportingEndpoint);
+
+    // LordOS: a document that asks for isolation with Document-Isolation-Policy and is itself served with an enforced
+    // COEP of require-corp (WebKit's only enforcing COEP: "credentialless" isn't implemented, so it never qualifies)
+    // is treated as COOP same-origin + COEP, WebKit's own cross-origin isolation: the navigation switches to a new,
+    // isolated browsing context group in a web process of its own (CrossOriginMode::Isolated, the only process where
+    // SharedArrayBuffer and high-precision timers exist), and crossOriginIsolated is true. Only from "unsafe-none" and
+    // "same-origin-allow-popups": both become stricter (popups to other origins lose their opener, as under COOP
+    // same-origin), which is what keeps every other origin out of that process now that WebKit has no per-document
+    // agent clusters. COEP and CORP checks of subresources are unchanged; the report-only COOP is untouched.
+    if ((policy.value == CrossOriginOpenerPolicyValue::UnsafeNone || policy.value == CrossOriginOpenerPolicyValue::SameOriginAllowPopups)
+        && documentIsolationPolicyIsolates(response) && ensureCOEP().value == CrossOriginEmbedderPolicyValue::RequireCORP)
+        policy.value = CrossOriginOpenerPolicyValue::SameOriginPlusCOEP;
     return policy;
 }
 
PATCH
    echo lordos-document-isolation-policy.patch >> debian/patches/series
    #    A service worker's notifications (+4.2, maintainer's request of 2026-11-26): registration.showNotification has no
    #    page, so WebKit's GLib port sent it to org.freedesktop.Notifications itself and the application never saw it (no
    #    sound rule, no silent, no window brought forward). Now it goes to the show-notification signal of a view of the
    #    worker's network session that has the worker's origin, like a page's notification, and a click or close there
    #    reaches the worker (notificationclick, notificationclose); with no such view, WebKit's D-Bus path as before. And a
    #    worker's clients.focus() reaches the application as the view's lordos-focus-from-service-worker signal
    #    (docs/WPEWEBKIT.md, "Service-worker notifications")
    cat > debian/patches/lordos-sw-notifications.patch <<'PATCH'
Description: a service worker's notifications reach a web view's show-notification; clients.focus() reaches the view
 WebKitNotificationProvider::show() had no page for a persistent notification (ServiceWorkerRegistration.showNotification)
 and sent it to org.freedesktop.Notifications from the UI process, so the application never saw it. Such a notification
 now goes to the show-notification signal of a WebKitWebView of the same network session whose page has the service
 worker's origin (a visible one first), like a page's notification; webkit_notification_clicked() and
 webkit_notification_close() reach the worker as notificationclick and notificationclose through the service workers'
 WebNotificationManagerProxy. Without such a view, or when no handler takes it, the D-Bus notification as before. A tag
 replaces only a notification of the same origin.
 WebPageProxy::focusFromServiceWorker (clients.focus(), WindowClient.focus()) did nothing in the GLib port: the view
 now emits lordos-focus-from-service-worker, for the application to bring its window forward; the worker's promise is
 answered at once, as before.
Author: LordOS <lord.basex@gmail.com>
Forwarded: no
Last-Update: 2026-11-26

--- a/Source/WebKit/UIProcess/API/glib/WebKitNotificationProvider.h
+++ b/Source/WebKit/UIProcess/API/glib/WebKitNotificationProvider.h
@@ -60,7 +60,7 @@
     void removeAPINotification(WebNotificationIdentifier);
     void closeAPINotification(WebNotificationIdentifier);
 
-    void withdrawAnyPreviousAPINotificationMatchingTag(const CString&);
+    void withdrawAnyPreviousAPINotificationMatchingTag(const CString&, const String& origin);
 
     void show(WebNotification&, const RefPtr<WebCore::NotificationResources>&);
 
--- a/Source/WebKit/UIProcess/API/glib/WebKitNotificationProvider.cpp
+++ b/Source/WebKit/UIProcess/API/glib/WebKitNotificationProvider.cpp
@@ -30,6 +30,7 @@
 #include "APIArray.h"
 #include "APINotificationProvider.h"
 #include "APINumber.h"
+#include "APISecurityOrigin.h"
 #include "NotificationService.h"
 #include "WebKitNotificationPrivate.h"
 #include "WebKitWebContextPrivate.h"
@@ -112,48 +113,60 @@
         webkit_notification_close(notification.get());
 }
 
-void WebKitNotificationProvider::withdrawAnyPreviousAPINotificationMatchingTag(const CString& tag)
+// LordOS: the origin a notification given to a view came from (a tag replaces only a notification of the same origin:
+// the service workers' provider holds every origin's)
+static const char* lordosNotificationOriginKey = "lordos-notification-origin";
+
+void WebKitNotificationProvider::withdrawAnyPreviousAPINotificationMatchingTag(const CString& tag, const String& origin)
 {
     if (!tag.length())
         return;
 
+    auto originUTF8 = origin.utf8();
     for (auto& notification : m_apiNotifications.values()) {
-        if (tag == webkit_notification_get_tag(notification.get())) {
+        auto* notificationOrigin = static_cast<const char*>(g_object_get_data(G_OBJECT(notification.get()), lordosNotificationOriginKey));
+        if (tag == webkit_notification_get_tag(notification.get()) && notificationOrigin && originUTF8 == notificationOrigin) {
             closeAPINotification(WebNotificationIdentifier { webkit_notification_get_id(notification.get()) });
             break;
         }
     }
-
-#if ASSERT_ENABLED
-    for (auto& notification : m_apiNotifications.values())
-        ASSERT(tag != webkit_notification_get_tag(notification.get()));
-#endif
 }
 
 void WebKitNotificationProvider::show(WebPageProxy* page, WebNotification& webNotification, RefPtr<WebCore::NotificationResources>&& resources)
 {
-    if (!page || !m_webContext) {
-        // FIXME: glib API needs to find their own solution to handling pageless notifications.
+    // LordOS: a service worker's notification (registration.showNotification: persistent, no page) goes to the
+    // show-notification signal of a view of the worker's network session that has the worker's origin, like a page's;
+    // the application's handler shows it, and webkit_notification_clicked() and webkit_notification_close() reach the
+    // worker as notificationclick and notificationclose (WebNotificationManagerProxy, as from D-Bus). Without such a
+    // view, or when no handler takes it, WebKit's own D-Bus notification as before.
+    WebKitWebView* webView = nullptr;
+    if (page && m_webContext)
+        webView = webkitWebContextGetWebViewForPage(m_webContext, page);
+    else if (!page && webNotification.isPersistentNotification())
+        webView = webkitWebContextGetWebViewForServiceWorkerNotification(webNotification.sessionID(), webNotification.origin().securityOrigin());
+    if (!webView) {
+        ASSERT(!page || !m_webContext);
         show(webNotification, resources);
         return;
     }
 
     GRefPtr<WebKitNotification> notification = m_apiNotifications.get(webNotification.identifier());
     if (!notification) {
-        withdrawAnyPreviousAPINotificationMatchingTag(webNotification.tag().utf8());
+        auto origin = webNotification.origin().securityOrigin().toString();
+        withdrawAnyPreviousAPINotificationMatchingTag(webNotification.tag().utf8(), origin);
         notification = adoptGRef(webkitNotificationCreate(webNotification));
+        g_object_set_data_full(G_OBJECT(notification.get()), lordosNotificationOriginKey, g_strdup(origin.utf8().data()), g_free);
         g_signal_connect(notification.get(), "closed", G_CALLBACK(apiNotificationCloseCallback), this);
         g_signal_connect(notification.get(), "clicked", G_CALLBACK(apiNotificationClickedCallback), this);
         m_apiNotifications.set(webNotification.identifier(), notification);
     }
 
-    auto* webView = webkitWebContextGetWebViewForPage(m_webContext, page);
-    ASSERT(webView);
-
     if (webkitWebViewEmitShowNotification(webView, notification.get()))
         m_notificationManager->providerDidShowNotification(webNotification.identifier());
     else {
         g_signal_handlers_disconnect_by_data(notification.get(), this);
+        // LordOS: not left in the map, so a later same-tag notification can't close it
+        m_apiNotifications.remove(webNotification.identifier());
         show(webNotification, resources);
     }
 }
--- a/Source/WebKit/UIProcess/API/glib/WebKitWebContextPrivate.h
+++ b/Source/WebKit/UIProcess/API/glib/WebKitWebContextPrivate.h
@@ -30,6 +30,8 @@
 #include "WebKitWebsitePolicies.h"
 #include "WebProcessPool.h"
 #include <WebCore/ResourceRequest.h>
+#include <WebCore/SecurityOriginData.h>
+#include <pal/SessionID.h>
 
 WebKit::WebProcessPool& webkitWebContextGetProcessPool(WebKitWebContext*);
 #if !ENABLE(2022_GLIB_API)
@@ -38,6 +40,7 @@
 void webkitWebContextWebViewCreated(WebKitWebContext*, WebKitWebView*);
 void webkitWebContextWebViewDestroyed(WebKitWebContext*, WebKitWebView*);
 WebKitWebView* webkitWebContextGetWebViewForPage(WebKitWebContext*, WebKit::WebPageProxy*);
+WebKitWebView* webkitWebContextGetWebViewForServiceWorkerNotification(PAL::SessionID, const WebCore::SecurityOriginData&);
 GVariant* webkitWebContextInitializeWebProcessExtensions(WebKitWebContext*);
 void webkitWebContextInitializeNotificationPermissions(WebKitWebContext*);
 #if ENABLE(REMOTE_INSPECTOR)
--- a/Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp
+++ b/Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp
@@ -58,6 +58,7 @@
 #include <JavaScriptCore/RemoteInspector.h>
 #include <WebCore/ContentSecurityPolicy.h>
 #include <WebCore/ResourceLoaderIdentifier.h>
+#include <WebCore/SecurityOriginData.h>
 #include <cstdlib>
 #include <glib/gi18n-lib.h>
 #include <libintl.h>
@@ -68,6 +69,7 @@
 #include <wtf/FileSystem.h>
 #include <wtf/HashMap.h>
 #include <wtf/HashSet.h>
+#include <wtf/ListHashSet.h>
 #include <wtf/NeverDestroyed.h>
 #include <wtf/RefCounted.h>
 #include <wtf/RefPtr.h>
@@ -2048,6 +2050,14 @@
     return *context->priv->processPool;
 }
 
+// LordOS: every web view of every context, in creation order: a service worker's notification has no page, and is shown
+// by a view of its network session that has its origin (webkitWebContextGetWebViewForServiceWorkerNotification)
+static ListHashSet<WebKitWebView*>& allWebViews()
+{
+    static NeverDestroyed<ListHashSet<WebKitWebView*>> webViews;
+    return webViews;
+}
+
 void webkitWebContextWebViewCreated(WebKitWebContext* context, WebKitWebView* webView)
 {
     Ref page = webkitWebViewGetPage(webView);
@@ -2057,14 +2067,38 @@
     }
 
     context->priv->webViews.set(page->identifier(), webView);
+    allWebViews().add(webView);
 }
 
 void webkitWebContextWebViewDestroyed(WebKitWebContext* context, WebKitWebView* webView)
 {
     context->priv->webViews.remove(webkitWebViewGetPage(webView).identifier());
+    allWebViews().remove(webView);
 }
 
 WebKitWebView* webkitWebContextGetWebViewForPage(WebKitWebContext* context, WebPageProxy* page)
 {
     return page ? context->priv->webViews.get(page->identifier()) : nullptr;
 }
+
+// LordOS: the view that shows a service worker's notification (registration.showNotification, which has no page): one
+// of the same network session (the worker's website data store) whose page has the worker's origin, a visible one
+// first; none (nullptr) keeps WebKit's own D-Bus notification
+WebKitWebView* webkitWebContextGetWebViewForServiceWorkerNotification(PAL::SessionID sessionID, const WebCore::SecurityOriginData& origin)
+{
+    if (origin.isOpaque())
+        return nullptr;
+    WebKitWebView* found = nullptr;
+    for (auto* webView : allWebViews()) {
+        auto& page = webkitWebViewGetPage(webView);
+        if (page.isClosed() || page.sessionID() != sessionID)
+            continue;
+        if (WebCore::SecurityOriginData::fromURLWithoutStrictOpaqueness(URL { page.currentURL() }) != origin)
+            continue;
+        if (page.isViewVisible())
+            return webView;
+        if (!found)
+            found = webView;
+    }
+    return found;
+}
--- a/Source/WebKit/UIProcess/API/glib/WebKitWebViewPrivate.h
+++ b/Source/WebKit/UIProcess/API/glib/WebKitWebViewPrivate.h
@@ -96,6 +96,7 @@
 void webkitWebViewHandleAuthenticationChallenge(WebKitWebView*, WebKit::AuthenticationChallengeProxy*);
 void webkitWebViewInsecureContentDetected(WebKitWebView*, WebKitInsecureContentEvent);
 bool webkitWebViewEmitShowNotification(WebKitWebView*, WebKitNotification*);
+void webkitWebViewFocusFromServiceWorker(WebKitWebView*);
 void webkitWebViewWebProcessTerminated(WebKitWebView*, WebKitWebProcessTerminationReason);
 void webkitWebViewIsPlayingAudioChanged(WebKitWebView*);
 void webkitWebViewMediaCaptureStateDidChange(WebKitWebView*, WebCore::MediaProducer::MediaStateFlags);
--- a/Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp
+++ b/Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp
@@ -209,6 +209,8 @@
 
     QUERY_PERMISSION_STATE,
 
+    LORDOS_FOCUS_FROM_SERVICE_WORKER,
+
     LAST_SIGNAL
 };
 
@@ -2588,6 +2590,18 @@
         G_TYPE_BOOLEAN, 1,
         WEBKIT_TYPE_NOTIFICATION);
 
+    // LordOS (not WebKit's API): WebKitWebView::lordos-focus-from-service-worker, emitted when a service worker asks
+    // for this view's page to be focused (clients.focus(), WindowClient.focus(): only while the worker handles a user
+    // gesture such as notificationclick). The application brings the view's window forward; the worker's promise
+    // resolves whatever it does.
+    signals[LORDOS_FOCUS_FROM_SERVICE_WORKER] = g_signal_new(
+        "lordos-focus-from-service-worker",
+        G_TYPE_FROM_CLASS(gObjectClass),
+        G_SIGNAL_RUN_LAST,
+        0, nullptr, nullptr,
+        g_cclosure_marshal_VOID__VOID,
+        G_TYPE_NONE, 0);
+
 #if PLATFORM(GTK)
      /**
       * WebKitWebView::run-color-chooser:
@@ -3220,6 +3234,11 @@
     return handled;
 }
 
+void webkitWebViewFocusFromServiceWorker(WebKitWebView* webView)
+{
+    g_signal_emit(webView, signals[LORDOS_FOCUS_FROM_SERVICE_WORKER], 0);
+}
+
 #if PLATFORM(GTK)
 bool webkitWebViewEmitRunColorChooser(WebKitWebView* webView, WebKitColorChooserRequest* request)
 {
--- a/Source/WebKit/UIProcess/API/glib/WebKitUIClient.cpp
+++ b/Source/WebKit/UIProcess/API/glib/WebKitUIClient.cpp
@@ -80,6 +80,15 @@
         webkitWebViewReadyToShowPage(m_webView);
     }
 
+    // LordOS: a service worker's clients.focus() (WebPageProxy::focusFromServiceWorker) reaches the application as
+    // WebKitWebView::lordos-focus-from-service-worker. False: WebKit answers the worker at once, as before, instead of
+    // waiting for the view to become visible (which an application that doesn't bring it forward would never do)
+    bool focusFromServiceWorker(WebPageProxy&) final
+    {
+        webkitWebViewFocusFromServiceWorker(m_webView);
+        return false;
+    }
+
     void close(WebPageProxy*) final
     {
         webkitWebViewClosePage(m_webView);
PATCH
    echo lordos-sw-notifications.patch >> debian/patches/series
    #    The DRM backend's own API (WPEDisplayDRM, WPEScreenDRM, WPEToplevelDRM, WPEViewDRM: wpe_display_drm_* and the
    #    _get_type of the others) isn't in the library then, so it leaves Debian's symbols file too; WPEPlatform's core
    #    DRM device API (wpe_drm_device_*, used for dmabuf) stays
    n=$(grep -cE '^ wpe_(display|screen|toplevel|view)_drm_' debian/libwpewebkit-2.0-1.symbols) || true
    [ "${n:-0}" -gt 0 ] || die "debian/libwpewebkit-2.0-1.symbols has no DRM backend symbols: check what changed"
    sed -i -E '/^ wpe_(display|screen|toplevel|view)_drm_/d' debian/libwpewebkit-2.0-1.symbols
    echo "symbols file: $n symbols of the DRM backend removed"
    # 2. LordOS maintains this build: bugs come to LordOS, not to Debian's WebKit team, who stay credited in the
    #    changelog and debian/copyright (no XSBC-Original-Maintainer: Debian's lintian doesn't know the field)
    grep -q '^Maintainer: ' debian/control || die "debian/control has no Maintainer"
    sed -i "s/^Maintainer: .*$/Maintainer: $DEBEMAIL_LORDOS/; /^Uploaders:/d" debian/control
    # 3. the changelog entry, dated one second after Debian's own entry it follows (a later date, as Debian policy
    #    asks, but a fixed one): the date is SOURCE_DATE_EPOCH for the build, so the same version built again gives
    #    the same .debian.tar.xz (and the same binaries, as far as WebKit's build allows)
    debdate=$(TZ=UTC date -R -d "@$(( $(dpkg-parsechangelog -S Timestamp) + 1 ))")
    DEBEMAIL=$DEBEMAIL_LORDOS dch --force-bad-version --newversion "$newver" --distribution trixie --force-distribution \
        "Backport to Debian 13 (trixie) for LordOS (docs/WPEWEBKIT.md in the LordOS repository)."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "Build WPEPlatform (wpe-platform-2.0: headless, Wayland) and keep the legacy libwpe API, both explicitly."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "Don't build WPEPlatform's DRM backend, so libinput and libudev aren't linked; its symbols leave debian/libwpewebkit-2.0-1.symbols."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "Build WebRTC with WebKit's GStreamer backend (webrtcbin) and MediaStream; Build-Depends on libssl-dev, Recommends the GStreamer plugins WebRTC uses (libnice, bad, good, base)."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "lordos-turn-transport-query.patch: TURN URLs with ?transport=udp or ?transport=tcp are accepted."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "ICE with librice (USE_LIBRICE) in the network process, so the web process keeps no network; Build-Depends on librice-dev (LordOS's package)."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "lordos-sharedarraybuffer-coi.patch: SharedArrayBuffer in cross-origin-isolated web processes only, as on Cocoa."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "lordos-document-isolation-policy.patch: Document-Isolation-Policy with COEP require-corp gives WebKit's cross-origin isolation (COOP same-origin + COEP)."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "lordos-sw-notifications.patch: a service worker's notifications go to a web view's show-notification (same network session and origin), D-Bus otherwise; clients.focus() emits lordos-focus-from-service-worker."
    DEBEMAIL=$DEBEMAIL_LORDOS dch --append "Maintainer: LordOS (the packaging is the Debian WebKit Maintainers')."
    sed -i "0,/^ -- /s/^\( -- .*>  \).*$/\1$debdate/" debian/changelog
    [ "$(dpkg-parsechangelog -S Date)" = "$debdate" ] || die "could not date the changelog entry"
}

cmd_build() {
    dver=$1 newver=$2
    up=${dver%-*}
    dsc=/build/in/wpewebkit_$dver.dsc
    verify_source "$dsc" "$up"

    id -u builder >/dev/null 2>&1 || useradd -m builder
    mkdir -p /build/work /build/out
    cp /build/in/wpewebkit_$up.orig.tar.xz /build/in/wpewebkit_$up.orig.tar.xz.asc /build/work/
    dpkg-source --no-check -x "$dsc" /build/work/wpewebkit-$up
    if [ "${WEBRTC_BACKEND:-gstreamer}" = libwebrtc ]; then
        # WebKit's own libwebrtc (+5): the source package's libwebrtc component, made outside from WebKit's git
        # (tools/pkg/wpewebkit-libwebrtc-source.sh) and checked here against the SHA-256 pinned in the LordOS tree
        comp=wpewebkit_$up.orig-libwebrtc.tar.xz
        want=$(awk -v u="$up" '$1 == "upstream" && $2 != u { exit 1 } $1 == "sha256" { print $2 }' /build/wpewebkit-libwebrtc.sums) ||
            die "wpewebkit-libwebrtc.sums doesn't pin libwebrtc for $up"
        [ "$(sha256sum < "/build/in/$comp" | cut -d' ' -f1)" = "$want" ] || die "$comp: not the pinned SHA-256 $want"
        echo "$comp: the pinned SHA-256"
        cp "/build/in/$comp" /build/work/
        tar -xJf "/build/work/$comp" -C /build/work/wpewebkit-$up
    fi
    cd /build/work/wpewebkit-$up
    backport "$newver"
    [ "${WEBRTC_BACKEND:-gstreamer}" != libwebrtc ] || sh /build/wpewebkit-libwebrtc/backport.sh /build/wpewebkit-libwebrtc
    # LordOS's own build dependencies (librice: tools/pkg/build-librice.sh), from /build/extra, then the rest from trixie
    # only (a missing one stops here and is named)
    if ls /build/extra/*.deb >/dev/null 2>&1; then apt-get install -y --no-install-recommends /build/extra/*.deb; fi
    mk-build-deps --install --remove --tool 'apt-get -y --no-install-recommends' debian/control
    rm -f wpewebkit-build-deps_*
    chown -R builder: /build/work
    start=$(date +%s)
    runuser -u builder -- env DEB_BUILD_OPTIONS="parallel=${JOBS:-$(nproc)}" dpkg-buildpackage -us -uc -sa
    end=$(date +%s)
    echo "build time: $((end - start)) s" | tee /build/out/build-time.txt
    cd /build/work
    # /build/out is the build machine's disk (a bind mount): the results are kept from here on, whatever happens next
    cp ./*.deb ./*.dsc ./*.debian.tar.xz ./*.orig.tar.xz ./*.orig.tar.xz.asc ./*.buildinfo /build/out/
    cp ./*.orig-*.tar.xz /build/out/ 2>/dev/null || true    # the libwebrtc component (+5)
    cp ./*.changes /build/out/    # last: a .changes in out/ means the rest is there
    sync
    # the source package alone, re-checked: every file the .dsc names is in /build/out and matches
    (cd /build/out && dscverify --no-sig-check "wpewebkit_${newver}.dsc")
    lintian --info --display-info --pedantic --profile debian /build/out/wpewebkit_${newver}_*.changes > /build/out/lintian.txt 2>&1 || true
    echo "lintian: $(grep -c '^E:' /build/out/lintian.txt) errors, $(grep -c '^W:' /build/out/lintian.txt) warnings," \
         "$(grep -c '^I:' /build/out/lintian.txt) info, $(grep -c '^P:' /build/out/lintian.txt) pedantic" | tee /build/out/lintian-summary.txt
    sed -i '/^$/d' /build/out/lintian-summary.txt
}

cmd_test() {
    out=/build/test
    mkdir -p "$out/old"
    # trixie's own 2.48 first, for the ABI comparison (never installed)
    (cd "$out/old" && apt-get download libwpewebkit-2.0-1 >/dev/null)
    dpkg-deb -x "$out"/old/libwpewebkit-2.0-1_*.deb "$out/old/root"
    apt-get install -y --no-install-recommends /build/debs/*.deb
    {
        echo "== installed"
        dpkg-query -W 'libwpewebkit*' wpewebkit-webdriver
        echo "== pkg-config"
        for m in wpe-webkit-2.0 wpe-web-process-extension-2.0 wpe-platform-2.0 wpe-platform-headless-2.0 \
                 wpe-platform-wayland-2.0 wpe-1.0 wpebackend-fdo-1.0; do
            v=$(pkg-config --modversion "$m") || die "pkg-config does not find $m"
            printf '%s %s\n' "$m" "$v"
        done
        echo "== SONAME"
        old=$(ls "$out"/old/root/usr/lib/*/libWPEWebKit-2.0.so.1.*)
        new=$(ls /usr/lib/*/libWPEWebKit-2.0.so.1.*)
        printf 'trixie 2.48: %s %s\nbackport:    %s %s\n' "${old##*/}" "$(objdump -p "$old" | awk '/SONAME/ { print $2 }')" \
            "${new##*/}" "$(objdump -p "$new" | awk '/SONAME/ { print $2 }')"
        nm -D --defined-only "$old" | awk '{ print $NF }' | sort -u > "$out/old.syms"
        nm -D --defined-only "$new" | awk '{ print $NF }' | sort -u > "$out/new.syms"
        echo "exported symbols: 2.48 $(wc -l < "$out/old.syms"), backport $(wc -l < "$out/new.syms")," \
             "removed $(comm -23 "$out/old.syms" "$out/new.syms" | wc -l), added $(comm -13 "$out/old.syms" "$out/new.syms" | wc -l)"
        comm -23 "$out/old.syms" "$out/new.syms" > "$out/removed.syms"
        echo "== NEEDED of the backport's library (readelf -d)"
        readelf -d "$new" | awk '/NEEDED/ { gsub(/[][]/, "", $5); print $5 }' | tr '\n' ' '; echo
        echo "== copyright files"
        for p in libwpewebkit-2.0-1 libwpewebkit-2.0-dev wpewebkit-webdriver libwpewebkit-doc; do
            if dpkg -s "$p" >/dev/null 2>&1; then ls -l "/usr/share/doc/$p/copyright"; fi
        done
        echo "== trixie packages that depend on libwpewebkit-2.0-1"
        apt-cache rdepends --installed=false libwpewebkit-2.0-1 | sed '1,2d' | sort -u
        echo "== WPEPlatform headless render"
    } > "$out/report.txt" 2>&1
    cat "$out/report.txt"
    # what the maintainer's decisions require: no DRM backend, so no libinput or libudev in WebKit's NEEDED
    ! pkg-config --exists wpe-platform-drm-2.0 || die "wpe-platform-drm-2.0 is installed: the DRM backend was built"
    lib=$(ls /usr/lib/*/libWPEWebKit-2.0.so.1.*)
    ! readelf -d "$lib" | grep -E 'NEEDED.*\[lib(input|udev)\.so' || die "libWPEWebKit-2.0 links libinput or libudev"
    echo "no libinput, no libudev in libWPEWebKit-2.0's NEEDED" | tee -a "$out/report.txt"
    if [ "${WEBRTC_BACKEND:-gstreamer}" = libwebrtc ]; then
        # WebKit's LibWebRTC backend (+5, maintainer's decision of 2026-11-26): libwebrtc with its BoringSSL, libvpx and
        # libaom is inside the library and must stay there: none of their symbols exported (hidden visibility), so
        # nothing else can link them; no GStreamer WebRTC, no librice; Linux's own libevent and libopus
        readelf -d "$lib" | grep -q 'NEEDED.*\[libevent-2\.1\.so' || die "libWPEWebKit-2.0 doesn't link libevent: libwebrtc isn't built"
        readelf -d "$lib" | grep -q 'NEEDED.*\[libopus\.so' || die "libWPEWebKit-2.0 doesn't link libopus: libwebrtc isn't built"
        ! readelf -d "$lib" | grep -E 'NEEDED.*\[lib(gstwebrtc-1\.0|rice-proto|rice-io)\.so' || die "the GStreamer WebRTC backend is linked too"
        nm -D --defined-only "$lib" | awk '{ print $NF }' | grep -E '^(SSL_|SSL_CTX_|EVP_|BIO_|X509_|RSA_|EC_|BN_|CRYPTO_|OPENSSL_|vpx_|vp8_|vp9_|aom_|av1_|srtp_|webrtc|_ZN6webrtc|_ZN3rtc|_ZN4absl)' > "$out/libwebrtc-exported.txt" || true
        [ ! -s "$out/libwebrtc-exported.txt" ] || { head "$out/libwebrtc-exported.txt"; die "libWPEWebKit-2.0 exports libwebrtc's, BoringSSL's, libvpx's or libaom's symbols"; }
        echo "WebRTC: LibWebRTC backend inside libWPEWebKit-2.0 (libevent, libopus in NEEDED; no BoringSSL, libvpx, libaom or libwebrtc symbol exported; no GStreamer WebRTC, no librice)" | tee -a "$out/report.txt"
    else
        # WebRTC built (maintainer's decision, 2026-11-25): WebKit's GStreamer backend links GStreamer's WebRTC library
        readelf -d "$lib" | grep -q 'NEEDED.*\[libgstwebrtc-1\.0\.so' || die "libWPEWebKit-2.0 doesn't link libgstwebrtc-1.0: WebRTC isn't built"
        echo "WebRTC: libgstwebrtc-1.0 in libWPEWebKit-2.0's NEEDED" | tee -a "$out/report.txt"
        readelf -d "$lib" | grep -q 'NEEDED.*\[librice-proto\.so' || die "libWPEWebKit-2.0 doesn't link librice-proto: ICE isn't librice's"
        echo "ICE: librice-proto and librice-io in libWPEWebKit-2.0's NEEDED (the network process runs ICE)" | tee -a "$out/report.txt"
    fi
    gcc -Wall -Wextra -O1 -o "$out/wpe-headless-render" /build/wpewebkit-headless-test.c \
        $(pkg-config --cflags --libs wpe-webkit-2.0 wpe-platform-2.0 wpe-platform-headless-2.0)
    id -u tester >/dev/null 2>&1 || useradd -m tester
    chown tester: "$out"
    grep -q 'WEBKIT_DISABLE_SANDBOX' /proc/self/environ && die "WebKit's sandbox must stay on in the test"
    # Under qemu-user (EMULATED=1: another architecture than the build machine's) WebKit can't run with its sandbox:
    # bubblewrap's user namespaces fail there, and the sandbox is never turned off, so the programs are only built;
    # running them needs a real machine of that architecture (docs/WPEWEBKIT.md, "arm64")
    if [ -n "${EMULATED:-}" ]; then
        echo "headless render: built; not run under qemu-user (needs a native $(dpkg --print-architecture) machine)" | tee -a "$out/report.txt"
    else
        # a user like any desktop user: WebKit starts its web and network processes in its bubblewrap sandbox
        runuser -u tester -- env HOME=/home/tester XDG_RUNTIME_DIR=/tmp timeout 120 "$out/wpe-headless-render" "$out/render.ppm" >> "$out/report.txt" 2>&1 ||
            { tail -n 30 "$out/report.txt"; die "the headless render failed"; }
        tail -n 5 "$out/report.txt"
    fi
    rm -rf "$out/old/root"
    [ ! -f /build/lordos-src.tar ] || lordweb_check
}

# today's lord-web against this WebKit, on both of its view backends (WPEPlatform and the legacy libwpe one): built
# with -Wall -Wextra, then its own headless backend test (server/tests/web_backend_test.c: render, input, resize,
# scale, visibility, cursor) and its probe test (server/tests/web_probe_test.py), both with WebKit's sandbox on
lordweb_check() {
    out=/build/test
    rm -rf /build/src && mkdir -p /build/src && tar -xf /build/lordos-src.tar -C /build/src
    chown -R tester: /build/src
    echo "== lord-web (LordOS $(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' /build/src/package.json | head -1))" >> "$out/report.txt"
    for b in platform legacy; do
        runuser -u tester -- make -C /build/src/server -j"$(nproc)" LORD_WEB_BACKEND=$b build/lord-web build/web-backend-test \
            > "$out/lord-web-$b.log" 2>&1 || { tail -n 30 "$out/lord-web-$b.log"; die "lord-web ($b) does not build"; }
        ! grep -E 'warning:' "$out/lord-web-$b.log" || die "lord-web ($b) builds with warnings"
        if [ -n "${EMULATED:-}" ]; then
            echo "lord-web ($b backend): builds without warnings; its tests not run under qemu-user" | tee -a "$out/report.txt"
            continue
        fi
        (cd /build/src/server && runuser -u tester -- env HOME=/home/tester XDG_RUNTIME_DIR=/tmp timeout 300 build/web-backend-test) \
            >> "$out/report.txt" 2>&1 || { tail -n 20 "$out/report.txt"; die "web-backend-test ($b) failed"; }
        (cd /build/src/server && runuser -u tester -- env HOME=/home/tester XDG_RUNTIME_DIR=/tmp timeout 300 python3 -I tests/web_probe_test.py build/lord-web --pss) \
            >> "$out/report.txt" 2>&1 || { tail -n 20 "$out/report.txt"; die "web_probe_test ($b) failed"; }
        echo "lord-web ($b backend): builds without warnings, web-backend-test and web_probe_test pass" | tee -a "$out/report.txt"
    done
    # WebRTC (maintainer's decision, 2026-11-25): on WPEPlatform, the call checks against this engine: Chrome's identity,
    # SharedArrayBuffer only when cross-origin isolated (lordos-sharedarraybuffer-coi.patch), and two peer connections in
    # one page exchanging audio and video (Opus, VP8 and H.264), with WebKit's sandbox on (ICE in the network process,
    # librice: the loopback must connect), and a page with a messaging web app's Document-Isolation-Policy headers
    # cross-origin isolated (lordos-document-isolation-policy.patch, --dip); a service worker's notifications reach the
    # view's show-notification, a click there its notificationclick, clients.focus() the view's signal, and with no view
    # of its origin they still go to D-Bus (lordos-sw-notifications.patch, --swnotify: a session bus of its own)
    if [ -z "${EMULATED:-}" ]; then
        runuser -u tester -- make -C /build/src/server -j"$(nproc)" LORD_WEB_BACKEND=platform build/webcalls-check > "$out/webcalls-build.log" 2>&1 ||
            { tail -n 30 "$out/webcalls-build.log"; die "webcalls-check does not build"; }
        ! grep -E 'warning:' "$out/webcalls-build.log" || die "webcalls-check builds with warnings"
        (cd /build/src/server && runuser -u tester -- env HOME=/home/tester XDG_RUNTIME_DIR=/tmp timeout 600 python3 -I tests/webcalls_check.py build/webcalls-check --webrtc --dip --swnotify) \
            > "$out/webcalls.txt" 2>&1 || { cat "$out/webcalls.txt"; die "webcalls_check failed"; }
        cat "$out/webcalls.txt" >> "$out/report.txt"
        echo "WebRTC: webcalls_check passes (identity, SharedArrayBuffer, Document-Isolation-Policy, service-worker notifications, loopback call)" | tee -a "$out/report.txt"
    fi
}

case "${1:-}" in
    build) shift; cmd_build "$@" ;;
    test) cmd_test ;;
    *) die "usage: wpewebkit-chroot.sh build <debian-version> <lordos-version> | test" ;;
esac
