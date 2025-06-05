/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/HTML/Window.h>
#include <LibCore/File.h>
#include <LibCore/DateTime.h>

namespace Web {

GC::Ref<JS::Realm> internal_css_realm()
{
    static GC::Root<JS::Realm> realm;
    static GC::Root<HTML::Window> window;
    static OwnPtr<JS::ExecutionContext> execution_context;
    if (!realm) {
        execution_context = Bindings::create_a_new_javascript_realm(
            Bindings::main_thread_vm(),
            [&](JS::Realm& realm) -> JS::Object* {
                window = HTML::Window::create(realm);
                return window;
            },
            [&](JS::Realm&) -> JS::Object* {
                return window;
            });

        realm = *execution_context->realm;
        auto intrinsics = realm->create<Bindings::Intrinsics>(*realm);
        auto host_defined = make<Bindings::HostDefined>(intrinsics);
        realm->set_host_defined(move(host_defined));
    }
    return *realm;
}

static String sanitize_filename(StringView url_string)
{
    // Replace characters that can't be used in filenames
    StringBuilder sanitized;
    for (auto ch : url_string) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            sanitized.append('_');
        } else {
            sanitized.append(ch);
        }
    }
    return MUST(sanitized.to_string());
}

static void write_css_to_file(StringView css, StringView source_description, Optional<::URL::URL> const& location)
{
    String filename;
    if (location.has_value()) {
        auto url_string = location->to_string();
        auto sanitized_url = sanitize_filename(url_string);
        filename = MUST(String::formatted("{}.csslog", sanitized_url));
    } else {
        filename = "inline.csslog"_string;
    }
    
    // Append to file (so we don't overwrite multiple CSS from same source)
    auto file_result = Core::File::open(filename, Core::File::OpenMode::Write | Core::File::OpenMode::Append);
    if (!file_result.is_error()) {
        auto file = file_result.release_value();
        
        // Write header
        auto header = MUST(String::formatted("/* CSS from: {} */\n/* ==================== */\n\n", source_description));
        (void)file->write_until_depleted(header.bytes());
        
        // Write CSS content
        (void)file->write_until_depleted(css.bytes());
        
        // Write separator
        (void)file->write_until_depleted("\n\n"sv.bytes());
        
        dbgln("CSS logged to file: {} (source: {})", filename, source_description);
    }
}

GC::Ref<CSS::CSSStyleSheet> parse_css_stylesheet(CSS::Parser::ParsingParams const& context, StringView css, Optional<::URL::URL> location, Vector<NonnullRefPtr<CSS::MediaQuery>> media_query_list)
{
    // Log CSS content to file with source information
    if (!css.is_empty()) {
        String source_description;
        if (location.has_value()) {
            source_description = MUST(String::formatted("External stylesheet: {}", location->to_string()));
        } else {
            source_description = "Inline CSS or style tag"_string;
        }
        
        write_css_to_file(css, source_description, location);
    }
    
    if (css.is_empty()) {
        auto rule_list = CSS::CSSRuleList::create(*context.realm);
        auto media_list = CSS::MediaList::create(*context.realm, {});
        auto style_sheet = CSS::CSSStyleSheet::create(*context.realm, rule_list, media_list, location);
        style_sheet->set_source_text({});
        return style_sheet;
    }
    auto style_sheet = CSS::Parser::Parser::create(context, css).parse_as_css_stylesheet(location, move(media_query_list));
    // FIXME: Avoid this copy
    style_sheet->set_source_text(MUST(String::from_utf8(css)));
    return style_sheet;
}

CSS::Parser::Parser::PropertiesAndCustomProperties parse_css_property_declaration_block(CSS::Parser::ParsingParams const& context, StringView css)
{
    if (css.is_empty())
        return {};
    return CSS::Parser::Parser::create(context, css).parse_as_property_declaration_block();
}

Vector<CSS::Descriptor> parse_css_descriptor_declaration_block(CSS::Parser::ParsingParams const& parsing_params, CSS::AtRuleID at_rule_id, StringView css)
{
    if (css.is_empty())
        return {};
    return CSS::Parser::Parser::create(parsing_params, css).parse_as_descriptor_declaration_block(at_rule_id);
}

RefPtr<CSS::CSSStyleValue const> parse_css_value(CSS::Parser::ParsingParams const& context, StringView string, CSS::PropertyID property_id)
{
    if (string.is_empty())
        return nullptr;
    return CSS::Parser::Parser::create(context, string).parse_as_css_value(property_id);
}

RefPtr<CSS::CSSStyleValue const> parse_css_descriptor(CSS::Parser::ParsingParams const& parsing_params, CSS::AtRuleID at_rule_id, CSS::DescriptorID descriptor_id, StringView string)
{
    if (string.is_empty())
        return nullptr;
    return CSS::Parser::Parser::create(parsing_params, string).parse_as_descriptor_value(at_rule_id, descriptor_id);
}

CSS::CSSRule* parse_css_rule(CSS::Parser::ParsingParams const& context, StringView css_text)
{
    return CSS::Parser::Parser::create(context, css_text).parse_as_css_rule();
}

Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    return CSS::Parser::Parser::create(context, selector_text).parse_as_selector();
}

Optional<CSS::SelectorList> parse_selector_for_nested_style_rule(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    auto parser = CSS::Parser::Parser::create(context, selector_text);

    auto maybe_selectors = parser.parse_as_relative_selector(CSS::Parser::Parser::SelectorParsingMode::Standard);
    if (!maybe_selectors.has_value())
        return {};

    return adapt_nested_relative_selector_list(*maybe_selectors);
}

Optional<CSS::PageSelectorList> parse_page_selector_list(CSS::Parser::ParsingParams const& params, StringView selector_text)
{
    return CSS::Parser::Parser::create(params, selector_text).parse_as_page_selector_list();
}

Optional<CSS::Selector::PseudoElementSelector> parse_pseudo_element_selector(CSS::Parser::ParsingParams const& context, StringView selector_text)
{
    return CSS::Parser::Parser::create(context, selector_text).parse_as_pseudo_element_selector();
}

RefPtr<CSS::MediaQuery> parse_media_query(CSS::Parser::ParsingParams const& context, StringView string)
{
    return CSS::Parser::Parser::create(context, string).parse_as_media_query();
}

Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(CSS::Parser::ParsingParams const& context, StringView string)
{
    return CSS::Parser::Parser::create(context, string).parse_as_media_query_list();
}

RefPtr<CSS::Supports> parse_css_supports(CSS::Parser::ParsingParams const& context, StringView string)
{
    if (string.is_empty())
        return {};
    return CSS::Parser::Parser::create(context, string).parse_as_supports();
}

}
