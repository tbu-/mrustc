/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * macro_rules/parse.cpp
 * - macro_rules! parsing
 */
#include <common.hpp>
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "macro_rules.hpp"
#include "pattern_checks.hpp"
#include <ast/crate.hpp>    // for editions

MacroRulesPtr Parse_MacroRules(TokenStream& lex);
namespace {
    ::std::vector<SimplePatEnt> macro_pattern_to_simple(const Span& sp, const ::std::vector<MacroPatEnt>& pattern);
}

/// A rule within a macro_rules! blcok
class MacroRule
{
public:
    ::std::vector<MacroPatEnt>  m_pattern;
    Span    m_pat_span;
    ::std::vector<MacroExpansionEnt> m_contents;

    MacroRule() {}
    MacroRule(MacroRule&&) = default;
    MacroRule(const MacroRule&) = delete;
};

/// Parse the pattern of a macro_rules! arm
::std::vector<MacroPatEnt> Parse_MacroRules_Pat(TokenStream& lex, enum eTokenType open, enum eTokenType close,  ::std::vector<RcString>& names)
{
    TRACE_FUNCTION;
    Token tok;

    ::std::vector<MacroPatEnt>  ret;

     int    depth = 0;
    auto ps = lex.start_span();
    while( GET_TOK(tok, lex) != close || depth > 0 )
    {
        DEBUG("tok = " << tok);
        if( tok.type() == open )
        {
            depth ++;
        }
        else if( tok.type() == close )
        {
            if(depth == 0)
                throw ParseError::Generic(FMT("Unmatched " << Token(close) << " in macro pattern"));
            depth --;
        }

        switch(tok.type())
        {
        case TOK_DOLLAR:
            switch( GET_TOK(tok, lex) )
            {
            // TODO: Allow any reserved word
            default:
                if( !(TOK_RWORD_PUB <= tok.type() && tok.type() <= TOK_RWORD_UNSIZED) )
                    throw ParseError::Unexpected(lex, tok);
            case TOK_IDENT: {
                auto name = tok.type() == TOK_IDENT ? tok.istr() : RcString::new_interned(tok.to_str());
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                RcString type = tok.istr();

                unsigned int idx = ::std::find( names.begin(), names.end(), name ) - names.begin();
                if( idx == names.size() )
                    names.push_back( name );

                auto sp = lex.end_span(ps);
                MacroPatEnt::Type   ty;
                if(0)
                    ;
                else if( type == "tt" )
                    ty = MacroPatEnt::PAT_TT;
                else if( type == "pat" )
                    ty = MacroPatEnt::PAT_PAT;
                else if( type == "ident" )
                    ty = MacroPatEnt::PAT_IDENT;
                else if( type == "path" )
                    ty = MacroPatEnt::PAT_PATH;
                else if( type == "expr" )
                    ty = MacroPatEnt::PAT_EXPR;
                else if( type == "stmt" )
                    ty = MacroPatEnt::PAT_STMT;
                else if( type == "ty" )
                    ty = MacroPatEnt::PAT_TYPE;
                else if( type == "meta" )
                    ty = MacroPatEnt::PAT_META;
                else if( type == "block" )
                    ty = MacroPatEnt::PAT_BLOCK;
                else if( type == "item" )
                    ty = MacroPatEnt::PAT_ITEM;
                else if( /*TARGETVER_1_29 && */ type == "vis" ) // TODO: Should this be selective?
                    ty = MacroPatEnt::PAT_VIS;
                else if( /*TARGETVER_1_29 && */ type == "lifetime" ) // TODO: Should this be selective?
                    ty = MacroPatEnt::PAT_LIFETIME;
                else if( /*TARGETVER_1_39 && */ type == "literal" ) // TODO: Should this be selective?
                    ty = MacroPatEnt::PAT_LITERAL;
                else
                    ERROR(lex.point_span(), E0000, "Unknown fragment type '" << type << "'");
                ret.push_back( MacroPatEnt(sp, name, idx, ty) );
                break; }
            case TOK_PAREN_OPEN: {
                auto subpat = Parse_MacroRules_Pat(lex, TOK_PAREN_OPEN, TOK_PAREN_CLOSE, names);
                enum eTokenType joiner = TOK_NULL;
                GET_TOK(tok, lex);
                if( lex.parse_state().edition_after(AST::Edition::Rust2018) && tok.type() == TOK_QMARK )
                {
                    // 2018 added `?` repetition operator
                }
                else if( tok.type() == TOK_PLUS || tok.type() == TOK_STAR )
                {
                    // `+` and `*` were present at 1.0 (2015)
                }
                else
                {
                    DEBUG("joiner = " << tok);
                    joiner = tok.type();
                    GET_TOK(tok, lex);
                }
                DEBUG("tok = " << tok);
                auto sp = lex.end_span(ps);
                switch(tok.type())
                {
                case TOK_PLUS:
                    DEBUG("$()+ " << subpat);
                    ret.push_back( MacroPatEnt(sp, Token(joiner), "+", ::std::move(subpat)) );
                    break;
                case TOK_STAR:
                    DEBUG("$()* " << subpat);
                    ret.push_back( MacroPatEnt(sp, Token(joiner), "*", ::std::move(subpat)) );
                    break;
                case TOK_QMARK:
                    DEBUG("$()? " << subpat);
                    ret.push_back( MacroPatEnt(sp, Token(joiner), "?", ::std::move(subpat)) );
                    break;
                default:
                    throw ParseError::Unexpected(lex, tok, { TOK_PLUS, TOK_STAR });
                }
                break; }
            }
            break;
        case TOK_EOF:
            throw ParseError::Unexpected(lex, tok);
        default:
            ret.push_back( MacroPatEnt(lex.end_span(ps), tok) );
            break;
        }
        ps = lex.start_span();
    }

    return ret;
}

/// Parse the contents (replacement) of a macro_rules! arm
::std::vector<MacroExpansionEnt> Parse_MacroRules_Cont(
    TokenStream& lex,
    enum eTokenType open, enum eTokenType close,
    const ::std::vector<RcString>& var_names,
    ::std::map<unsigned int,bool>* var_set_ptr/*=nullptr*/
    )
{
    TRACE_FUNCTION;

    Token tok;
    ::std::vector<MacroExpansionEnt> ret;

     int    depth = 0;
    while( GET_TOK(tok, lex) != close || depth > 0 )
    {
        if( tok.type() == TOK_EOF ) {
            throw ParseError::Unexpected(lex, tok);
        }
        if( tok.type() == TOK_NULL )
            continue ;

        if( tok.type() == open )
        {
            DEBUG("depth++");
            depth ++;
        }
        else if( tok.type() == close )
        {
            DEBUG("depth--");
            if(depth == 0)
                ERROR(lex.point_span(), E0000, "Unmatched " << Token(close) << " in macro content");
            depth --;
        }

        // `$` - Macro metavars
        if( tok.type() == TOK_DOLLAR )
        {
            GET_TOK(tok, lex);

            // `$(`
            if( tok.type() == TOK_PAREN_OPEN )
            {
                ::std::map<unsigned int, bool> var_set;
                auto content = Parse_MacroRules_Cont(lex, TOK_PAREN_OPEN, TOK_PAREN_CLOSE, var_names, &var_set);
                // ^^ The above will eat the PAREN_CLOSE

                GET_TOK(tok, lex);
                enum eTokenType joiner = TOK_NULL;
                bool is_optional;
                if( lex.parse_state().edition_after(AST::Edition::Rust2018) && tok.type() == TOK_QMARK )
                {
                    // 2018 added `?` repetition operator
                }
                else if( tok.type() == TOK_PLUS || tok.type() == TOK_STAR )
                {
                    // `+` and `*` were present at 1.0 (2015)
                }
                else
                {
                    joiner = tok.type();
                    is_optional = false;
                    GET_TOK(tok, lex);
                }
                switch(tok.type())
                {
                case TOK_PLUS:
                    is_optional = false; if(0)
                case TOK_STAR:
                    is_optional = true; if(0)
                case TOK_QMARK:
                    is_optional = true;
                    // TODO: Ensure that +/* match up?
                    break;
                default:
                    throw ParseError::Unexpected(lex, tok);
                }
                if( var_set_ptr ) {
                    for(const auto& v : var_set) {
                        // If `is_optional`: Loop may not be expanded, so var_not_opt=false
                        // Else, inherit
                        bool var_not_opt = (is_optional ? false : v.second);
                        var_set_ptr->insert( ::std::make_pair(v.first, var_not_opt) ).first->second |= var_not_opt;
                    }
                }
                DEBUG("joiner = " << Token(joiner) << ", var_set = " << var_set << ", content = " << content);
                ret.push_back( MacroExpansionEnt::make_Loop({mv$(content), joiner, mv$(var_set)}) );
            }
            else if( tok.type() == TOK_RWORD_CRATE )
            {
                ret.push_back( MacroExpansionEnt( (1<<30) | 0 ) );
            }
            //else if( tok.type() == TOK_IDENT || tok_is_rword(tok.type()) )
            else if( tok.type() == TOK_IDENT || tok.type() >= TOK_RWORD_PUB )
            {
                // Look up the named parameter in the list of param names for this arm
                auto name = tok.type() == TOK_IDENT ? tok.istr() : RcString::new_interned(tok.to_str());
                unsigned int idx = ::std::find(var_names.begin(), var_names.end(), name) - var_names.begin();
                if( idx == var_names.size() ) {
                    // TODO: `error-chain`'s quick_error macro has an arm which refers to an undefined metavar.
                    // - Maybe emit a warning and use a marker index.
                    // NOTE: No warning emitted, it's just noise...
                    //WARNING(lex.point_span(), W0000, "Macro variable $" << name << " not found");

                    // Emit the literal $ <name>
                    ret.push_back( MacroExpansionEnt(Token(TOK_DOLLAR)) );
                    ret.push_back( MacroExpansionEnt(mv$(tok)) );
                    continue ;
                }
                if( var_set_ptr ) {
                    var_set_ptr->insert( ::std::make_pair(idx,true) );
                }
                ret.push_back( MacroExpansionEnt(idx) );
            }
            else
            {
                throw ParseError::Unexpected(lex, tok);
            }
        }
        else
        {
            ret.push_back( MacroExpansionEnt( mv$(tok) ) );
        }
    }

    return ret;
}

/// Parse a single arm of a macro_rules! block - `(foo) => (bar)`
MacroRule Parse_MacroRules_Var(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;

    MacroRule   rule;

    // Pattern
    enum eTokenType close;
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    case TOK_SQUARE_OPEN:   close = TOK_SQUARE_CLOSE;   break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    // - Pattern entries
    ::std::vector<RcString>   names;
    {
        auto ps = lex.start_span();
        rule.m_pattern = Parse_MacroRules_Pat(lex, tok.type(), close,  names);
        rule.m_pat_span = lex.end_span(ps);
    }

    GET_CHECK_TOK(tok, lex, TOK_FATARROW);

    // Replacement
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    rule.m_contents = Parse_MacroRules_Cont(lex, tok.type(), close, names);

    DEBUG("Rule - ["<<rule.m_pattern<<"] => "<<rule.m_contents<<"");

    return rule;
}

// TODO: Also count the number of times each variable is used?
void enumerate_names(const ::std::vector<MacroPatEnt>& pats, ::std::vector<RcString>& names)
{
    for( const auto& pat : pats )
    {
        if( pat.type == MacroPatEnt::PAT_LOOP ) {
            enumerate_names(pat.subpats, names);
        }
        else if( pat.name != "" ) {
            auto b = names.begin();
            auto e = names.end();
            if( ::std::find(b, e, pat.name) == e ) {
                names.push_back( pat.name );
            }
        }
    }
}

MacroRulesArm Parse_MacroRules_MakeArm(Span pat_sp, ::std::vector<MacroPatEnt> pattern, ::std::vector<MacroExpansionEnt> contents)
{
    // - Convert the rule into an instruction stream
    auto rule_sequence = macro_pattern_to_simple(pat_sp, pattern);
    auto arm = MacroRulesArm( mv$(rule_sequence), mv$(contents) );
    enumerate_names(pattern,  arm.m_param_names);
    return arm;
}

/// Parse an entire macro_rules! block into a format that exec.cpp can use
MacroRulesPtr Parse_MacroRules(TokenStream& lex)
{
    TRACE_FUNCTION_F("");

    Token tok;

    // Parse the patterns and replacements
    ::std::vector<MacroRule>    rules;
    while( lex.lookahead(0) != TOK_EOF && lex.lookahead(0) != TOK_BRACE_CLOSE )
    {
        rules.push_back( Parse_MacroRules_Var(lex) );
        if(GET_TOK(tok, lex) != TOK_SEMICOLON) {
            PUTBACK(tok,lex);
            break;
        }
    }
    GET_TOK(tok, lex);
    if(tok.type() != TOK_EOF && tok.type() != TOK_BRACE_CLOSE)
        throw ParseError::Unexpected(lex, tok, { TOK_EOF, TOK_BRACE_CLOSE });
    DEBUG("- " << rules.size() << " rules");

    auto rv = MacroRulesPtr(new MacroRules( ));
    rv->m_hygiene = lex.getHygiene();
    //rv->m_rules = mv$(rule_arms);

    // Re-parse the patterns into a unified form
    for(auto& rule : rules)
    {
        rv->m_rules.push_back( Parse_MacroRules_MakeArm(rule.m_pat_span, mv$(rule.m_pattern), mv$(rule.m_contents)) );
    }

    return rv;
}

namespace {

    struct ExpTok {
        MacroPatEnt::Type   ty;
        const Token* tok;
        ExpTok(MacroPatEnt::Type ty, const Token* tok): ty(ty), tok(tok) {}
        bool operator==(const ExpTok& t) const { return this->ty == t.ty && *this->tok == *t.tok; }
        bool operator!=(const ExpTok& t) const { return !(*this == t); }
    };
    ::std::ostream& operator<<(::std::ostream& os, const ExpTok& t) {
        os << "ExpTok(" << t.ty << " " << *t.tok << ")";
        return os;
    }

    // Yields all possible ExpectTok/ExpectPat entries from a pattern position
    // Returns `true` if there's no fall-through
    /// rv: Output vector for tokens
    /// pattern: Input pattern (match arm or body of a repeat)
    /// direct_pos: Position in `pattern` at which to start the search
    /// indirect_path: Token/check path to follow before returning an item
    /// indirect_ofs: Current offset into `indirect_path`
    bool macro_pattern_get_head_set_inner(
        ::std::vector<ExpTok>& rv, const ::std::vector<MacroPatEnt>& pattern, size_t direct_pos,
        const std::vector<ExpTok>& indirect_path, size_t indirect_ofs
        )
    {
        for(size_t idx = direct_pos; idx < pattern.size(); idx ++)
        {
            const auto& ent = pattern[idx];
            switch(ent.type)
            {
            case MacroPatEnt::PAT_LOOP:
                if( macro_pattern_get_head_set_inner(rv, ent.subpats, 0, indirect_path, indirect_ofs) )
                {
                    // + loops have to iterate at least once, so if the set is closed by the sub-patterns, close us too
                    if(ent.name == "+")
                    {
                        return true;
                    }
                    else if( ent.name == "*" || ent.name == "?" )
                    {
                        // for * and ? loops, they can be skipped entirely.
                        // - No separator, this is for the skip case
                    }
                    else
                    {
                        BUG(Span(), "Unknown loop type " << ent.name);
                    }
                }
                else
                {
                    if( ent.name != "?" )
                    {
                        assert(ent.subpats.size() > 0);
                        macro_pattern_get_head_set_inner(rv, ent.subpats, 0, indirect_path, indirect_ofs+ent.subpats.size());
                    }

                    // If the inner pattern didn't close the option set, then the next token can be the separator
                    if( ent.tok != TOK_NULL )
                    {
                        // If indirect is non-zero, decrement without doing anything
                        if( indirect_ofs < indirect_path.size() )
                        {
                            if( indirect_path[indirect_ofs] != ExpTok(MacroPatEnt::PAT_TOKEN, &ent.tok) )
                            {
                                return false;
                            }
                            indirect_ofs ++;
                        }
                        else
                        {
                            rv.push_back(ExpTok(MacroPatEnt::PAT_TOKEN, &ent.tok));
                        }
                    }
                    else
                    {
                    }
                }
                break;
            default:
                if( indirect_ofs < indirect_path.size() )
                {
                    if( indirect_path[indirect_ofs] != ExpTok(ent.type, &ent.tok) )
                    {
                        return false;
                    }
                    indirect_ofs ++;
                }
                else
                {
                    rv.push_back( ExpTok(ent.type, &ent.tok) );
                    return true;
                }
                break;
            }
        }
        return false;
    }
    ::std::vector<ExpTok> macro_pattern_get_head_set(const ::std::vector<MacroPatEnt>& pattern, size_t direct_pos, const std::vector<ExpTok>& indirect_path)
    {
        ::std::vector<ExpTok>   rv;
        // If the pattern set isn't closed (hit something unconditional), then add `EOF` to it
        if( !macro_pattern_get_head_set_inner(rv, pattern, direct_pos, indirect_path, 0) )
        {
            if(rv.empty())
            {
                static Token    tok_eof = TOK_EOF;
                rv.push_back(ExpTok(MacroPatEnt::PAT_TOKEN, &tok_eof));
            }
        }
        return rv;
    }

    void macro_pattern_to_simple_inner(const Span& sp, ::std::vector<SimplePatEnt>& rv, const ::std::vector<MacroPatEnt>& pattern)
    {
        size_t level_start = rv.size();
        TRACE_FUNCTION_FR("[" << pattern << "]", "[" << FMT_CB(ss, for(auto it = rv.begin()+level_start; it != rv.end(); ++it) { ss << *it << ", "; }) << "]");
        auto push = [&rv](SimplePatEnt spe) {
            DEBUG("[macro_pattern_to_simple_inner] rv[" << rv.size() << "] = " << spe);
            rv.push_back( ::std::move(spe) );
            };
        auto push_ifv = [&push](bool is_equal, ::std::vector<SimplePatIfCheck> ents, size_t tgt) {
            push(SimplePatEnt::make_If({ is_equal, tgt, mv$(ents) }));
            };
        auto push_if = [&push_ifv](bool is_equal, MacroPatEnt::Type ty, const Token& tok, size_t tgt) {
            push_ifv(is_equal, make_vec1(SimplePatIfCheck { ty, tok }), tgt);
            };
        for(size_t idx = 0; idx < pattern.size(); idx ++)
        {
            const auto& ent = pattern[idx];
            DEBUG("[" << idx << "] ent = " << ent);
            switch(ent.type)
            {
            case MacroPatEnt::PAT_LOOP: {
                auto entry_pats1 = macro_pattern_get_head_set(ent.subpats, 0, {});
                DEBUG("Entry = [" << entry_pats1 << "]");
                ASSERT_BUG(ent.sp, entry_pats1.size() > 0, "No entry conditions extracted from sub-pattern [" << ent.subpats << "]");
                auto skip_pats1 = macro_pattern_get_head_set(pattern, idx+1, {});
                DEBUG("Skip = [" << skip_pats1 << "]");

                std::vector<std::vector<SimplePatIfCheck>>   entry_conds;
                std::vector<std::vector<SimplePatIfCheck>>   skip_conds;
                std::vector<std::vector<SimplePatIfCheck>>   repeat_conds;

                for(const auto& ee : entry_pats1)
                {
                    entry_conds.push_back(::make_vec1<SimplePatIfCheck>({ ee.ty, *ee.tok }));
                }
                for(const auto& ee : skip_pats1)
                {
                    skip_conds.push_back(::make_vec1<SimplePatIfCheck>({ ee.ty, *ee.tok }));
                }

                // - Duplicates need special handling (build up a subseqent set)
                for(auto e_it = entry_conds.begin(); e_it != entry_conds.end(); ++e_it)
                {
                    auto s_it = ::std::find(skip_conds.begin(), skip_conds.end(), *e_it);
                    if( s_it != skip_conds.end() )
                    {
                        auto path = ::make_vec1<ExpTok>(ExpTok(e_it->front().ty, &e_it->front().tok));
                        auto entry_pats2 = macro_pattern_get_head_set(ent.subpats, 0, path);
                        assert(entry_pats2.size() > 0);
                        auto skip_pats2 = macro_pattern_get_head_set(pattern, idx+1, path);
                        assert(skip_pats2.size() > 0);
                        DEBUG("entry_pats2 = [" << entry_pats2 << "]");
                        DEBUG("skip_pats2 = [" << skip_pats2 << "]");
                        // Update the current element for both of them, and add new elements to the end of each list
                        {
                            auto e2_it = entry_pats2.begin();
                            e_it->push_back({e2_it->ty, *e2_it->tok});
                            for(++e2_it; e2_it != entry_pats2.end(); ++ e2_it)
                            {
                                e_it = entry_conds.insert(e_it, *e_it);
                                e_it->back() = SimplePatIfCheck { e2_it->ty, *e2_it->tok };
                            }
                        }

                        {
                            auto s2_it = skip_pats2.begin();
                            s_it->push_back({s2_it->ty, *s2_it->tok});
                            for(++s2_it; s2_it != skip_pats2.end(); ++ s2_it)
                            {
                                s_it = skip_conds.insert(s_it, *s_it);
                                s_it->back() = SimplePatIfCheck { s2_it->ty, *s2_it->tok };
                            }
                        }
                        //TODO(ent.sp, "Entry and skip patterns share an entry, ambigious - " << path);
                    }
                }
                // - If there's three-level needed, error?
                for(auto e_it = entry_conds.begin(); e_it != entry_conds.end(); ++e_it)
                {
                    auto s_it = ::std::find(skip_conds.begin(), skip_conds.end(), *e_it);
                    if( s_it != skip_conds.end() )
                    {
                        TODO(ent.sp, "Entry and skip patterns share an entry, ambigious - " << *e_it);
                    }
                }
                DEBUG("Entry = [" << entry_conds << "]");
                DEBUG("Skip = [" << skip_conds << "]");

                // - Generate the repeat condition set
                if( ent.tok != TOK_NULL )
                {
                    // NOTE: If the separator is also allowed after the list, then this can't just check for the separator
                    for(const auto& p : entry_conds)
                    {
                        auto v = ::make_vec1<SimplePatIfCheck>( { MacroPatEnt::PAT_TOKEN, ent.tok} );
                        v.insert(v.end(), p.begin(), p.end());
                        repeat_conds.push_back(mv$(v));
                    }
                }

                // TODO: Combine the two cases below into one?

                // If the loop is a $()+ loop, then just recurse into it
                if( ent.name == "+" )
                {
                    push( SimplePatEnt::make_LoopStart({}) );
                    size_t start = rv.size();
                    macro_pattern_to_simple_inner(sp, rv, ent.subpats);
                    push( SimplePatEnt::make_LoopNext({}) );
                    size_t rewrite_start = rv.size();
                    if( ent.tok != TOK_NULL )
                    {
                        if(repeat_conds.size() > 1) {
                            size_t expect_and_jump_pos = rv.size() + entry_conds.size() + 1;
                            for(const auto& ee : repeat_conds) {
                                push_ifv(true, ee, expect_and_jump_pos);
                            }
                            // If any of the above succeeded, they'll jump past this jump to the ExpectTok
                            push( SimplePatEnt::make_Jump({ ~0u }) );
                        }
                        else {
                            push_ifv( false, repeat_conds.front(), ~0u );
                        }
                        push( SimplePatEnt::make_ExpectTok(ent.tok) );
                        push( SimplePatEnt::make_Jump({ start }) );
                    }
                    else
                    {
                        // TODO: What if there's a collision at this level?
                        for(const auto& p : entry_conds)
                        {
                            push_ifv(true, p, start);
                        }
                    }

                    size_t post_loop = rv.size();
                    for(size_t i = rewrite_start; i < post_loop; i++)
                    {
                        if( auto* pe = rv[i].opt_If() ) {
                            if(pe->jump_target == ~0u) {
                                pe->jump_target = post_loop;
                            }
                        }
                        if( auto* pe = rv[i].opt_Jump() ) {
                            if(pe->jump_target == ~0u) {
                                pe->jump_target = post_loop;
                            }
                        }
                    }
                    push( SimplePatEnt::make_LoopEnd({}) );
                }
                else if( ent.name == "*" || ent.name == "?" )
                {
                    push( SimplePatEnt::make_LoopStart({}) );

                    // Options:
                    // - Enter the loop (if the next token is one of the head set of the loop)
                    // - Skip the loop (the next token is the head set of the subsequent entries)
                    size_t rewrite_start = rv.size();
                    if( entry_conds.size() == 1 )
                    {
                        // If not the entry pattern, skip.
                        push_ifv(false, entry_conds.front(), ~0u);
                    }
                    else if( skip_conds.empty() )
                    {
                        // No skip patterns, try all entry patterns
                        size_t start = rv.size() + entry_conds.size() + 1;
                        for(const auto& p : entry_conds)
                        {
                            push_ifv(true, p, start);
                        }
                        push(SimplePatEnt::make_Jump({ ~0u }));
                    }
                    else
                    {
                        for(const auto& p : skip_conds)
                        {
                            push_ifv(true, p, ~0u);
                        }
                    }

                    macro_pattern_to_simple_inner(sp, rv, ent.subpats);
                    push( SimplePatEnt::make_LoopNext({}) );

                    if( ent.name == "*" )
                    {
                        if( ent.tok != TOK_NULL )
                        {
                            if( repeat_conds.size() == 1 )
                            {
                                // If not a repeat, jump out
                                for(const auto& ee : repeat_conds) {
                                    push_ifv(/*is_equal*/false, ee, ~0u);
                                }
                                push( SimplePatEnt::make_ExpectTok(ent.tok) );
                            }
                            else
                            {
                                // Multiple repeat conditions
                                // - If any repeat condition matches, then jump to a consume
                                auto check_pos = rv.size() + repeat_conds.size() + 1;
                                for(const auto& ee : repeat_conds) {
                                    push_ifv(/*is_equal*/true, ee, check_pos);
                                }
                                // - If none of the above matched, then jump out of the loop
                                push(SimplePatEnt::make_Jump({ ~0u }));
                                assert(rv.size() == check_pos);
                                push( SimplePatEnt::make_ExpectTok(ent.tok) );
                            }
                        }
                        // Jump back to the entry check.
                        push(SimplePatEnt::make_Jump({ rewrite_start }));
                    }
                    else if( ent.name == "?" )
                    {
                        ASSERT_BUG(sp, ent.tok == TOK_NULL, "$()? with a separator isn't valid");
                    }
                    else
                    {
                        BUG(sp, "");
                    }
                    size_t post_loop = rv.size();
                    for(size_t i = rewrite_start; i < post_loop; i++)
                    {
                        if( auto* pe = rv[i].opt_If() ) {
                            if(pe->jump_target == ~0u) {
                                pe->jump_target = post_loop;
                            }
                        }
                        if( auto* pe = rv[i].opt_Jump() ) {
                            if(pe->jump_target == ~0u) {
                                pe->jump_target = post_loop;
                            }
                        }
                    }
                    push( SimplePatEnt::make_LoopEnd({}) );
                }
                else
                {
                    TODO(sp, "Handle loop type '" << ent.name << "'");
                }
                } break;
            case MacroPatEnt::PAT_TOKEN:
                push( SimplePatEnt::make_ExpectTok(ent.tok) );
                break;
            default:
                push( SimplePatEnt::make_ExpectPat({ ent.type, ent.name_index }) );
                break;
            }
        }

        for(size_t i = level_start; i < rv.size(); i ++)
        {
            TU_MATCH_HDRA( (rv[i]), { )
            default:
                // Ignore
            TU_ARMA(If, e) {
                ASSERT_BUG(sp, e.jump_target < rv.size(), "If target out of bounds, " << e.jump_target << " >= " << rv.size());
                }
            TU_ARMA(Jump, e) {
                ASSERT_BUG(sp, e.jump_target < rv.size(), "Jump target out of bounds, " << e.jump_target << " >= " << rv.size());
                }
            }
        }
    }

    ::std::vector<SimplePatEnt> macro_pattern_to_simple(const Span& sp, const ::std::vector<MacroPatEnt>& pattern)
    {
        ::std::vector<SimplePatEnt> rv;
        TRACE_FUNCTION_FR(pattern, rv);
        macro_pattern_to_simple_inner(sp, rv, pattern);
        return rv;
    }
}
