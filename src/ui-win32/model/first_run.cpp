#include "model/first_run.h"

#include <algorithm>
#include <array>

namespace floserver::ui {

namespace {

using nlohmann::json;

std::string text_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

std::uint64_t count_of(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) {
        return 0;
    }
    const json& v = j[key];
    if (v.is_number_unsigned()) {
        return v.get<std::uint64_t>();
    }
    if (v.is_number_integer() && v.get<std::int64_t>() > 0) {
        return static_cast<std::uint64_t>(v.get<std::int64_t>());
    }
    return 0;
}

struct OfferSpec {
    const char* name;
    const char* series;  // "" for the newest of any
    Str what;
    bool ticked;
    bool always;
};

// The approved list, in the page's order.
constexpr std::array kOffers{
    OfferSpec{"php", "8.5", Str::FirstRunWhatPhp, true, false},
    OfferSpec{"mariadb", "11.4", Str::FirstRunWhatMariadb, true, false},
    OfferSpec{"node", "24", Str::FirstRunWhatNode, true, false},
    OfferSpec{"composer", "", Str::FirstRunWhatComposer, true, false},
    OfferSpec{"python", "3.13", Str::FirstRunWhatPython, false, false},
    OfferSpec{"phpmyadmin", "5.2", Str::FirstRunWhatPma, false, false},
    OfferSpec{"apache", "2.4", Str::FirstRunWhatApache, false, false},
    OfferSpec{"nginx", "", Str::FirstRunWhatNginx, true, true},
};

bool in_series(std::string_view version, std::string_view series) {
    return series.empty() || version == series ||
           (version.size() > series.size() && version.starts_with(series) && version[series.size()] == '.');
}

// What the row calls it: its series, or for one picked without a series the
// part people name it by (Composer 2, nginx 1.30).
std::string shown_series(std::string_view name, std::string_view series, std::string_view version) {
    if (!series.empty()) {
        return std::string(series);
    }
    if (name == "composer") {
        return std::string(version.substr(0, version.find('.')));
    }
    return version_series(name, version);
}

std::string join(const std::vector<std::string>& parts, std::string_view between) {
    std::string out;
    for (const std::string& p : parts) {
        out += (out.empty() ? "" : std::string(between)) + p;
    }
    return out;
}

}  // namespace

bool first_run_forced(const char* value) { return value != nullptr && std::string_view(value) == "1"; }

bool first_run_wanted(bool forced, bool done, const Installed& installed, std::size_t projects) {
    if (forced) {
        return true;
    }
    if (done || projects > 0) {
        return false;
    }
    return std::all_of(installed.begin(), installed.end(), [](const auto& kv) { return kv.second.empty(); });
}

std::string first_run_step_label(FirstRunStep step, FirstRunStep current, Lang lang) {
    const Str name = step == FirstRunStep::Language   ? Str::FirstRunStepLanguage
                     : step == FirstRunStep::Runtimes ? Str::FirstRunStepRuntimes
                                                      : Str::FirstRunStepReady;
    if (static_cast<int>(step) < static_cast<int>(current)) {
        return "\xE2\x9C\x93 " + std::string(text(lang, name));  // U+2713 CHECK MARK
    }
    return std::to_string(static_cast<int>(step) + 1) + " " + std::string(text(lang, name));
}

std::vector<FirstRunOffer> first_run_offers(const json& runtimes_reply) {
    std::vector<FirstRunOffer> out;
    const json& list = runtimes_reply.is_object() && runtimes_reply.contains("list") ? runtimes_reply["list"] : json();
    if (!list.is_object() || !runtimes_reply.contains("available") || !runtimes_reply["available"].is_array()) {
        return out;
    }
    const json& available = runtimes_reply["available"];
    for (const OfferSpec& spec : kOffers) {
        std::optional<FirstRunOffer> best;
        for (const json& r : available) {
            const std::string variant = text_of(r, "variant");
            const std::string version = text_of(r, "version");
            // Only PHP's usual build (the service installs nothing else).
            if (text_of(r, "name") != spec.name || version.empty() || !(variant.empty() || variant == "nts") ||
                !in_series(version, spec.series)) {
                continue;
            }
            if (best && !is_newer_version(version, best->version)) {
                continue;
            }
            FirstRunOffer o;
            o.name = spec.name;
            o.version = version;
            o.what = spec.what;
            o.size = count_of(r, "size");
            o.ticked = spec.ticked;
            o.always = spec.always;
            best = std::move(o);
        }
        if (best) {
            best->label = runtime_label(best->name) + " " + shown_series(best->name, spec.series, best->version);
            out.push_back(std::move(*best));
        }
    }
    return out;
}

std::vector<bool> first_run_default_ticks(const std::vector<FirstRunOffer>& offers) {
    std::vector<bool> out;
    out.reserve(offers.size());
    for (const FirstRunOffer& o : offers) {
        out.push_back(o.ticked || o.always);
    }
    return out;
}

namespace {

bool is_ticked(const std::vector<FirstRunOffer>& offers, const std::vector<bool>& ticks, std::size_t i) {
    return offers[i].always || (i < ticks.size() ? ticks[i] : offers[i].ticked);
}

}  // namespace

std::vector<Download> first_run_downloads(const std::vector<FirstRunOffer>& offers, const std::vector<bool>& ticks) {
    std::vector<Download> out;
    for (const FirstRunOffer& o : offers) {
        if (o.always) {
            out.push_back({o.name, o.version});
        }
    }
    for (std::size_t i = 0; i < offers.size(); ++i) {
        if (!offers[i].always && is_ticked(offers, ticks, i)) {
            out.push_back({offers[i].name, offers[i].version});
        }
    }
    return out;
}

std::uint64_t first_run_total(const std::vector<FirstRunOffer>& offers, const std::vector<bool>& ticks) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < offers.size(); ++i) {
        if (is_ticked(offers, ticks, i)) {
            total += offers[i].size;
        }
    }
    return total;
}

FirstRunListAnswer first_run_list_answer(const std::optional<json>& reply, const std::string& error, bool refreshed,
                                         Lang lang) {
    FirstRunListAnswer out;
    const auto failed = [&](const std::string& why) {
        out.kind = FirstRunListAnswer::Kind::Failed;
        out.error = fill(lang, Str::FirstRunListFailed, {why});
    };
    if (!reply || !reply->is_object()) {
        failed(error.empty() ? std::string("no answer") : error);
        return out;
    }
    const bool listed = reply->contains("list") && (*reply)["list"].is_object();
    if (!listed) {
        if (!refreshed) {
            out.kind = FirstRunListAnswer::Kind::Refresh;  // none on this computer yet: fetched once
            return out;
        }
        const std::string why = text_of(*reply, "list_error");
        failed(why.empty() ? std::string("no list") : why);
        return out;
    }
    out.offers = first_run_offers(*reply);
    if (out.offers.empty()) {
        out.kind = FirstRunListAnswer::Kind::Failed;
        out.error = std::string(text(lang, Str::FirstRunNoRuntimes));
        return out;
    }
    out.kind = FirstRunListAnswer::Kind::Offers;
    return out;
}

bool is_first_run_key(std::string_view key) { return key == kFirstRunKey; }

std::vector<FirstRunRow> first_run_rows(const std::vector<Download>& wanted, const Installed& installed,
                                        const std::vector<RuntimeJob>& jobs,
                                        const std::map<std::string, FirstRunEnd>& ends, Lang lang) {
    std::vector<FirstRunRow> out;
    out.reserve(wanted.size());
    for (const Download& d : wanted) {
        FirstRunRow row;
        row.name = d.name;
        row.label = runtime_label(d.name) + (d.version.empty() ? "" : " " + d.version);
        const auto have = installed.find(d.name);
        const bool ready =
            have != installed.end() &&
            (d.version.empty() ? !have->second.empty()
                               : std::find(have->second.begin(), have->second.end(), d.version) != have->second.end());
        const auto job = std::find_if(jobs.begin(), jobs.end(), [&](const RuntimeJob& j) {
            return j.name == d.name && (d.version.empty() || j.version == d.version);
        });
        const auto end = ends.find(d.name);
        if (ready) {
            row.state = FirstRunRowState::Ready;
            row.line = std::string(text(lang, Str::FirstRunRowReady));
        } else if (job != jobs.end() && job->state != "queued") {
            row.state = FirstRunRowState::Running;
            row.line = job_text(*job, lang);
            row.fraction = job_fraction(*job);
            row.job = job->id;
        } else if (job == jobs.end() && end != ends.end()) {
            const bool cancelled = end->second.state == "cancelled";
            row.state = cancelled ? FirstRunRowState::Cancelled : FirstRunRowState::Failed;
            row.line = cancelled ? std::string(text(lang, Str::FirstRunRowCancelled))
                                 : fill(lang, Str::FirstRunRowFailed, {end->second.message});
        } else {
            row.state = FirstRunRowState::Waiting;  // queued, or its install on its way
            row.line = std::string(text(lang, Str::FirstRunRowWaiting));
        }
        out.push_back(std::move(row));
    }
    return out;
}

FirstRunProgress first_run_progress(const std::vector<FirstRunRow>& rows) {
    FirstRunProgress p;
    p.total = rows.size();
    for (const FirstRunRow& r : rows) {
        if (r.state == FirstRunRowState::Ready) {
            ++p.ready;
        } else if (r.state == FirstRunRowState::Failed || r.state == FirstRunRowState::Cancelled) {
            ++p.failed;
        }
    }
    return p;
}

std::string first_run_runtimes_line(const std::vector<FirstRunRow>& rows, Lang lang) {
    if (rows.empty()) {
        return std::string(text(lang, Str::FirstRunNothingYet));
    }
    const bool rtl = is_rtl(lang);
    std::vector<std::string> ready;
    std::vector<std::string> failed;
    for (const FirstRunRow& r : rows) {
        if (r.state == FirstRunRowState::Ready) {
            ready.push_back(r.label);
        } else if (r.state == FirstRunRowState::Failed || r.state == FirstRunRowState::Cancelled) {
            failed.push_back(r.label);
        }
    }
    const FirstRunProgress p = first_run_progress(rows);
    std::vector<std::string> parts;
    if (!ready.empty()) {
        const std::string names = join(ready, ", ");
        parts.push_back(rtl ? isolate_ltr(names) : names);
    }
    if (p.running()) {
        parts.push_back(fill(lang, Str::FirstRunStillDownloading, {std::to_string(p.ready), std::to_string(p.total)}));
    }
    if (!failed.empty()) {
        parts.push_back(fill(lang, Str::FirstRunDidntDownload, {join(failed, ", ")}));
    }
    return join(parts, " · ");
}

std::string first_run_https_line(FirstRunTrust trust, const std::string& error, Lang lang) {
    switch (trust) {
        case FirstRunTrust::Trusted:
            return std::string(text(lang, Str::FirstRunTrusted));
        case FirstRunTrust::Checking:
        case FirstRunTrust::Asking:
            return std::string(text(lang, Str::FirstRunTrustAsking));
        case FirstRunTrust::Declined:
            return std::string(text(lang, Str::FirstRunTrustNo));
        case FirstRunTrust::Failed:
            return fill(lang, Str::FirstRunTrustFailed, {error});
        case FirstRunTrust::NotAsked:
            break;
    }
    return std::string(text(lang, Str::FirstRunNotTrusted));
}

}  // namespace floserver::ui
