// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/url_request/url_request_view_net_internals_job.h"

#if defined(OS_WIN)
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#include <sstream>

#include "base/stl_util-inl.h"
#include "base/string_util.h"
#include "net/base/escape.h"
#include "net/base/host_cache.h"
#include "net/base/load_log_util.h"
#include "net/base/net_errors.h"
#include "net/base/net_util.h"
#include "net/proxy/proxy_service.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/view_cache_helper.h"

namespace {

const char kViewHttpCacheSubPath[] = "view-cache";

//------------------------------------------------------------------------------
// Format helpers.
//------------------------------------------------------------------------------

void OutputTextInPre(const std::string& text, std::string* out) {
  out->append("<pre>");
  out->append(EscapeForHTML(text));
  out->append("</pre>");
}

//------------------------------------------------------------------------------
// Subsection definitions.
//------------------------------------------------------------------------------

class SubSection {
 public:
  // |name| is the URL path component for this subsection.
  // |title| is the textual description.
  SubSection(SubSection* parent, const char* name, const char* title)
      : parent_(parent),
        name_(name),
        title_(title) {
  }

  virtual ~SubSection() {
    STLDeleteContainerPointers(children_.begin(), children_.end());
  }

  // Outputs the subsection's contents to |out|.
  virtual void OutputBody(URLRequestContext* context, std::string* out) {}

  // Outputs this subsection, and all of its children.
  void OutputRecursive(URLRequestContext* context,
                       URLRequestViewNetInternalsJob::URLFormat* url_format,
                       std::string* out) {
    if (!is_root()) {
      // Canonicalizing the URL escapes characters which cause problems in HTML.
      std::string section_url =
          url_format->MakeURL(GetFullyQualifiedName()).spec();

      // Print the heading.
      out->append(StringPrintf("<div>"
          "<span class=subsection_title>%s</span> "
          "<span class=subsection_name>(<a href='%s'>%s</a>)<span>"
          "</div>",
          EscapeForHTML(title_).c_str(),
          section_url.c_str(),
          EscapeForHTML(section_url).c_str()));

      out->append("<div class=subsection_body>");
    }

    OutputBody(context, out);

    for (size_t i = 0; i < children_.size(); ++i)
      children_[i]->OutputRecursive(context, url_format, out);

    if (!is_root())
      out->append("</div>");
  }

  // Returns the SubSection contained by |this| with fully qualified name
  // |dotted_name|, or NULL if none was found.
  SubSection* FindSubSectionByName(const std::string& dotted_name) {
    if (dotted_name == "")
      return this;

    std::string child_name;
    std::string child_sub_name;

    size_t split_pos = dotted_name.find('.');
    if (split_pos == std::string::npos) {
      child_name = dotted_name;
      child_sub_name = std::string();
    } else {
      child_name = dotted_name.substr(0, split_pos);
      child_sub_name = dotted_name.substr(split_pos + 1);
    }

    for (size_t i = 0; i < children_.size(); ++i) {
      if (children_[i]->name_ == child_name)
        return children_[i]->FindSubSectionByName(child_sub_name);
    }

    return NULL;  // Not found.
  }

  std::string GetFullyQualifiedName() {
    if (!parent_)
      return name_;

    std::string parent_name = parent_->GetFullyQualifiedName();
    if (parent_name.empty())
      return name_;

    return parent_name + "." + name_;
  }

  bool is_root() const {
    return parent_ == NULL;
  }

 protected:
  typedef std::vector<SubSection*> SubSectionList;

  void AddSubSection(SubSection* subsection) {
    children_.push_back(subsection);
  }

  SubSection* parent_;
  std::string name_;
  std::string title_;

  SubSectionList children_;
};

class ProxyServiceCurrentConfigSubSection : public SubSection {
 public:
  ProxyServiceCurrentConfigSubSection(SubSection* parent)
      : SubSection(parent, "config", "Current configuration") {
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    net::ProxyService* proxy_service = context->proxy_service();
    if (proxy_service->config_has_been_initialized()) {
      // net::ProxyConfig defines an operator<<.
      std::ostringstream stream;
      stream << proxy_service->config();
      OutputTextInPre(stream.str(), out);
    } else {
      out->append("<i>Not yet initialized</i>");
    }
  }
};

class ProxyServiceLastInitLogSubSection : public SubSection {
 public:
  ProxyServiceLastInitLogSubSection(SubSection* parent)
      : SubSection(parent, "init_log", "Last initialized load log") {
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    net::ProxyService* proxy_service = context->proxy_service();
    net::LoadLog* log = proxy_service->init_proxy_resolver_log();
    if (log) {
      OutputTextInPre(net::LoadLogUtil::PrettyPrintAsEventTree(log), out);
    } else {
      out->append("<i>None.</i>");
    }
  }
};

class ProxyServiceBadProxiesSubSection : public SubSection {
 public:
  ProxyServiceBadProxiesSubSection(SubSection* parent)
      : SubSection(parent, "bad_proxies", "Bad Proxies") {
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    out->append("TODO");
  }
};

class ProxyServiceSubSection : public SubSection {
 public:
  ProxyServiceSubSection(SubSection* parent)
      : SubSection(parent, "proxyservice", "ProxyService") {
    AddSubSection(new ProxyServiceCurrentConfigSubSection(this));
    AddSubSection(new ProxyServiceLastInitLogSubSection(this));
    AddSubSection(new ProxyServiceBadProxiesSubSection(this));
  }
};

class HostResolverCacheSubSection : public SubSection {
 public:
  HostResolverCacheSubSection(SubSection* parent)
      : SubSection(parent, "hostcache", "HostCache") {
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    const net::HostCache* host_cache = context->host_resolver()->GetHostCache();

    if (!host_cache || host_cache->caching_is_disabled()) {
      out->append("<i>Caching is disabled.</i>");
      return;
    }

    out->append(StringPrintf("<ul><li>Size: %u</li>"
                             "<li>Capacity: %u</li>"
                             "<li>Time to live (ms): %u</li></ul>",
                             host_cache->size(),
                             host_cache->max_entries(),
                             host_cache->cache_duration_ms()));

    out->append("<table border=1>"
                "<tr>"
                "<th>Host</th>"
                "<th>Address family</th>"
                "<th>Address list</th>"
                "<th>Time to live (ms)</th>"
                "</tr>");

    for (net::HostCache::EntryMap::const_iterator it =
             host_cache->entries().begin();
         it != host_cache->entries().end();
         ++it) {
      const net::HostCache::Key& key = it->first;
      const net::HostCache::Entry* entry = it->second.get();

      std::string address_family_str =
          AddressFamilyToString(key.address_family);

      if (entry->error == net::OK) {
        // Note that ttl_ms may be negative, for the cases where entries have
        // expired but not been garbage collected yet.
        int ttl_ms = static_cast<int>(
            (entry->expiration - base::TimeTicks::Now()).InMilliseconds());

        // Color expired entries blue.
        if (ttl_ms > 0) {
          out->append("<tr>");
        } else {
          out->append("<tr style='color:blue'>");
        }

        // Stringify all of the addresses in the address list, separated
        // by newlines (br).
        std::string address_list_html;
        const struct addrinfo* current_address = entry->addrlist.head();
        while (current_address) {
          if (!address_list_html.empty())
            address_list_html += "<br>";
          address_list_html += EscapeForHTML(
              net::NetAddressToString(current_address));
          current_address = current_address->ai_next;
        }

        out->append(StringPrintf("<td>%s</td><td>%s</td><td>%s</td>"
                                 "<td>%d</td></tr>",
                                 EscapeForHTML(key.hostname).c_str(),
                                 EscapeForHTML(address_family_str).c_str(),
                                 address_list_html.c_str(),
                                 ttl_ms));
      } else {
        // This was an entry that failed to be resolved.
        // Color negative entries red.
        out->append(StringPrintf(
            "<tr style='color:red'><td>%s</td><td>%s</td>"
            "<td colspan=2>%s</td></tr>",
            EscapeForHTML(key.hostname).c_str(),
            EscapeForHTML(address_family_str).c_str(),
            EscapeForHTML(net::ErrorToString(entry->error)).c_str()));
      }
    }

    out->append("</table>");
  }

  static std::string AddressFamilyToString(net::AddressFamily address_family) {
    switch (address_family) {
      case net::ADDRESS_FAMILY_IPV4_ONLY:
        return "IPV4_ONLY";
      default:
        return "UNSPECIFIED";
    }
  }
};

class HostResolverSubSection : public SubSection {
 public:
  HostResolverSubSection(SubSection* parent)
      : SubSection(parent, "hostresolver", "HostResolver") {
    AddSubSection(new HostResolverCacheSubSection(this));
  }
};

// Helper for the URLRequest "outstanding" and "live" sections.
void OutputURLAndLoadLog(const GURL& url,
                         const net::LoadLog* log,
                         std::string* out) {
  out->append("<li>");
  out->append("<nobr>");
  out->append(EscapeForHTML(url.possibly_invalid_spec()));
  out->append("</nobr>");
  if (log)
    OutputTextInPre(net::LoadLogUtil::PrettyPrintAsEventTree(log), out);
  out->append("</li>");
}

class URLRequestLiveSubSection : public SubSection {
 public:
  URLRequestLiveSubSection(SubSection* parent)
      : SubSection(parent, "outstanding", "Outstanding requests") {
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    std::vector<URLRequest*> requests =
        context->request_tracker()->GetLiveRequests();

    out->append("<ol>");
    for (size_t i = 0; i < requests.size(); ++i) {
      // Reverse the list order, so we dispay from most recent to oldest.
      size_t index = requests.size() - i - 1;
      OutputURLAndLoadLog(requests[index]->original_url(),
                          requests[index]->load_log(),
                          out);
    }
    out->append("</ol>");
  }
};

class URLRequestRecentSubSection : public SubSection {
 public:
  URLRequestRecentSubSection(SubSection* parent)
      : SubSection(parent, "recent", "Recently completed requests") {
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    URLRequestTracker::RecentRequestInfoList recent =
        context->request_tracker()->GetRecentlyDeceased();

    out->append("<ol>");
    for (size_t i = 0; i < recent.size(); ++i) {
      // Reverse the list order, so we dispay from most recent to oldest.
      size_t index = recent.size() - i - 1;
      OutputURLAndLoadLog(recent[index].original_url,
                          recent[index].load_log, out);
    }
    out->append("</ol>");
  }
};

class URLRequestSubSection : public SubSection {
 public:
  URLRequestSubSection(SubSection* parent)
      : SubSection(parent, "urlrequest", "URLRequest") {
    AddSubSection(new URLRequestLiveSubSection(this));
    AddSubSection(new URLRequestRecentSubSection(this));
  }
};

class HttpCacheStatsSubSection : public SubSection {
 public:
  HttpCacheStatsSubSection(SubSection* parent)
      : SubSection(parent, "stats", "Statistics") {
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    ViewCacheHelper::GetStatisticsHTML(context, out);
  }
};

class HttpCacheSection : public SubSection {
 public:
  HttpCacheSection(SubSection* parent)
      : SubSection(parent, "httpcache", "HttpCache") {
    AddSubSection(new HttpCacheStatsSubSection(this));
  }

  virtual void OutputBody(URLRequestContext* context, std::string* out) {
    // Advertise the view-cache URL (too much data to inline it).
    out->append("<p><a href='/");
    out->append(kViewHttpCacheSubPath);
    out->append("'>View all cache entries</a></p>");
  }
};

class AllSubSections : public SubSection {
 public:
  AllSubSections() : SubSection(NULL, "", "") {
    AddSubSection(new ProxyServiceSubSection(this));
    AddSubSection(new HostResolverSubSection(this));
    AddSubSection(new URLRequestSubSection(this));
    AddSubSection(new HttpCacheSection(this));
  }
};

// Returns true if |path| is a subpath for "view-cache".
// If it is, then |key| is assigned the subpath.
bool GetViewCacheKeyFromPath(const std::string path,
                             std::string* key) {
  if (!StartsWithASCII(path, kViewHttpCacheSubPath, true))
    return false;

  if (path.size() > strlen(kViewHttpCacheSubPath) &&
      path[strlen(kViewHttpCacheSubPath)] != '/')
    return false;

  if (path.size() > strlen(kViewHttpCacheSubPath) + 1)
    *key = path.substr(strlen(kViewHttpCacheSubPath) + 1);

  return true;
}

}  // namespace

bool URLRequestViewNetInternalsJob::GetData(std::string* mime_type,
                                            std::string* charset,
                                            std::string* data) const {
  mime_type->assign("text/html");
  charset->assign("UTF-8");

  URLRequestContext* context = request_->context();
  std::string details = url_format_->GetDetails(request_->url());

  data->clear();

  // Use a different handler for "view-cache/*" subpaths.
  std::string cache_key;
  if (GetViewCacheKeyFromPath(details, &cache_key)) {
    GURL url = url_format_->MakeURL(kViewHttpCacheSubPath + std::string("/"));
    ViewCacheHelper::GetEntryInfoHTML(cache_key, context, url.spec(), data);
    return true;
  }

  data->append("<!DOCTYPE HTML>"
               "<html><head><title>Network internals</title>"
               "<style>"
               "body { font-family: sans-serif; font-size: 0.8em; }\n"
               "tt, code, pre { font-family: WebKitHack, monospace; }\n"
               ".subsection_body { margin: 10px 0 10px 2em; }\n"
               ".subsection_title { font-weight: bold; }\n"
               "</style>"
               "</head><body>"
               "<p><a href='http://dev.chromium.org/"
               "developers/design-documents/view-net-internals'>"
               "Help: how do I use this?</a></p>");

  SubSection* all = Singleton<AllSubSections>::get();
  SubSection* section = all;

  // Display only the subsection tree asked for.
  if (!details.empty())
    section = all->FindSubSectionByName(details);

  if (section) {
    section->OutputRecursive(context, url_format_, data);
  } else {
    data->append("<i>Nothing found for \"");
    data->append(EscapeForHTML(details));
    data->append("\"</i>");
  }

  data->append("</body></html>");

  return true;
}

