#include "code_generator.h"
#include "config_loader.h"
#include "control_flow.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBase = 0x80010000u;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

fs::path write_temp_config(const char* stem, const std::string& body) {
    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    (std::string(stem) + "-" + std::to_string(nonce) + ".toml");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return path;
}

std::string base_config() {
    return R"toml([game]
name = "Signed Bound Test"
id = "TEST-00000"
exe = "TEST.EXE"
load_address = "0x80010000"
entry_pc = "0x80010000"
text_size = "0x1000"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
)toml";
}

bool load_throws_with(const std::string& body, const char* needle) {
    fs::path path = write_temp_config("signed-x-bound", body);
    bool matched = false;
    try {
        (void)PSXRecompV4::load_game_config(path);
    } catch (const std::exception& e) {
        matched = std::string(e.what()).find(needle) != std::string::npos;
    }
    fs::remove(path);
    return matched;
}

PSXRecomp::GeneratedFunction generate_first_instruction(
    uint32_t first_word, const PSXRecomp::CodeGenConfig& config,
    uint32_t start_addr = kBase) {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = start_addr;
    exe.header.initial_pc = start_addr;
    exe.header.file_size = 20;
    append_word(exe.code_data, first_word);
    append_word(exe.code_data, 0x00000000u);
    append_word(exe.code_data, 0x24030001u);
    append_word(exe.code_data, 0x03E00008u);
    append_word(exe.code_data, 0x00000000u);

    PSXRecomp::Function function{};
    function.start_addr = start_addr;
    function.end_addr = start_addr + 20u;
    function.size = 20u;
    function.name = "signed_bound_test";

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg);
}

void loader_accepts_lui_and_addiu() {
    fs::path path = write_temp_config("signed-x-bound-valid", base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x3C020001"

[[widescreen.signed_x_bound]]
address = "0x80010004"
expected = "0x2402FF00"
)toml");
    auto config = PSXRecompV4::load_game_config(path);
    check(config.ws_signed_x_bound_sites.size() == 2,
          "loader accepts LUI plus ADDIU signed_x_bound entries");
    fs::remove(path);
}

void loader_rejects_bad_addiu_shapes() {
    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x30020100"
)toml", "expected must be LUI or ADDIU"),
          "loader rejects unsupported signed_x_bound opcode");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x20020100"
)toml", "expected must be LUI or ADDIU"),
          "loader rejects ADDI without a matching codegen/interpreter route");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x2482FF00"
)toml", "must use rs=$zero"),
          "loader rejects ADDIU signed_x_bound with nonzero rs");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x2400FF00"
)toml", "must not write $zero"),
          "loader rejects ADDIU signed_x_bound writing zero");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x24020000"
)toml", "non-zero signed screen edge"),
          "loader rejects ADDIU signed_x_bound with zero immediate");
}

void codegen_routes_bound_kinds() {
    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    config.ws_signed_x_bound_sites.push_back(
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x2402FF00u});
    auto addiu_neg = generate_first_instruction(0x2402FF00u, config).full_code;
    check(addiu_neg.find(
              "cpu->gpr[2] = (uint32_t)psx_ws_screen_x_bound(-256);") !=
              std::string::npos,
          "codegen routes ADDIU negative screen bound to screen helper");

    config.ws_signed_x_bound_sites[0] =
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x24020100u};
    auto addiu_pos = generate_first_instruction(0x24020100u, config).full_code;
    check(addiu_pos.find(
              "cpu->gpr[2] = (uint32_t)psx_ws_screen_x_bound(256);") !=
              std::string::npos,
          "codegen routes ADDIU positive screen bound to screen helper");

    config.ws_signed_x_bound_sites[0] =
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x3402FF00u};
    auto ori = generate_first_instruction(0x3402FF00u, config).full_code;
    check(ori.find("psx_ws_screen_x_bound(65280)") != std::string::npos,
          "ORI bound zero-extends, unlike the ADDIU negative bound");

    config.ws_signed_x_bound_sites[0] =
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x3C020001u};
    auto lui = generate_first_instruction(0x3C020001u, config).full_code;
    check(lui.find(
              "cpu->gpr[2] = (uint32_t)psx_ws_player_x_bound((int32_t)0x00010000);") !=
              std::string::npos,
          "codegen preserves LUI signed-Q16 gameplay helper");
}

std::string sxy_site(uint32_t address, uint32_t expected,
                     const std::string& fields = "", size_t folds = 0,
                     bool quad = false) {
    std::ostringstream out;
    out << "\n[[widescreen.cull.sxy]]\n"
        << "kind = \"" << (quad ? "quad" : "tri") << "\"\n"
        << "final_address = \"0x" << std::hex << address << "\"\n"
        << "final_expected = \"0x" << expected << "\"\n"
        << "vertex_regs = " << (quad ? "[4, 5, 6, 7]" : "[4, 5, 6]")
        << "\n" << fields;
    if (folds != 0) {
        out << "fold_addresses = [";
        for (size_t i = 0; i < folds; ++i)
            out << (i ? ", " : "") << "\"0x" << address + 4u * (i + 1u)
                << "\"";
        out << "]\nfold_expected = [";
        for (size_t i = 0; i < folds; ++i)
            out << (i ? ", " : "") << "\"0x00850824\"";
        out << "]\n";
    }
    return out.str();
}

void loader_infers_sxy_result_register() {
    for (uint32_t reg : {0u, 2u, 31u}) {
        for (bool quad : {false, true}) {
            const uint32_t expected = 0x00850024u | (reg << 11);
            const auto body = base_config() +
                             sxy_site(kBase, expected, "", 0, quad);
            const auto path = write_temp_config("sxy-inferred-result", body);
            try {
                const auto inferred = PSXRecompV4::load_game_config(path);
                check(inferred.ws_cull_sxy_sites.at(0).result_reg == reg,
                      "omitted SXY result_reg is inferred from final AND rd");
                const auto explicit_path = write_temp_config(
                    "sxy-explicit-result",
                    body + "result_reg = " + std::to_string(reg) + "\n");
                const auto explicit_config = PSXRecompV4::load_game_config(explicit_path);
                check(PSXRecompV4::overlay_codegen_config_hash(inferred) ==
                          PSXRecompV4::overlay_codegen_config_hash(explicit_config),
                      "inferred and explicit SXY result registers hash identically");
                fs::remove(explicit_path);
            } catch (const std::exception&) {
                check(false, "loader accepts omitted SXY result_reg for any AND rd");
            }
            fs::remove(path);
        }
    }
    check(load_throws_with(base_config() +
              sxy_site(kBase, 0x00851024u, "result_reg = 1\n"),
              "does not match final_expected destination 2"),
          "explicit SXY result_reg must equal final AND rd");
    for (int reg : {-1, 32}) {
        check(load_throws_with(base_config() + sxy_site(
                  kBase, 0x00851024u, "result_reg = " + std::to_string(reg) + "\n"),
                  "must be in [0, 31]"),
              "explicit SXY result_reg must remain a valid GPR");
    }
    check(load_throws_with(base_config() + sxy_site(kBase, 0x00851025u),
                          "sxy.final_expected must be AND"),
          "inferring SXY result_reg does not bypass final opcode validation");
}

void loader_bounds_sxy_sites_and_folds() {
    for (size_t folds : {0u, 4u, 5u}) {
        const auto body = base_config() + sxy_site(kBase, 0x00850824u, "", folds);
        if (folds > 4) {
            check(load_throws_with(body, "sxy supports at most 4 folds"),
                  "loader rejects SXY folds beyond runtime capacity");
        } else {
            const auto path = write_temp_config("sxy-fold-boundary", body);
            const auto config = PSXRecompV4::load_game_config(path);
            check(config.ws_cull_sxy_sites.at(0).fold_addresses.size() == folds,
                  "loader preserves all SXY folds through capacity");
            fs::remove(path);
        }
    }
    check(load_throws_with(base_config() + sxy_site(kBase, 0x00850824u,
              "fold_addresses = [\"0x80010004\"]\nfold_expected = []\n"),
              "must have the same size"),
          "SXY fold arrays still require matching sizes");
    for (size_t count : {64u, 65u}) {
        std::string body = base_config();
        for (size_t i = 0; i < count; ++i)
            body += sxy_site(kBase + 4u * i, 0x00850824u);
        if (count > 64) {
            check(load_throws_with(body, "sxy supports at most 64 sites"),
                  "loader rejects SXY sites beyond runtime capacity");
        } else {
            const auto path = write_temp_config("sxy-site-boundary", body);
            const auto config = PSXRecompV4::load_game_config(path);
            check(config.ws_cull_sxy_sites.size() == count,
                  "loader preserves all 64 SXY sites");
            fs::remove(path);
        }

        std::ostringstream lower;
        lower << base_config();
        for (size_t i = 0; i < count; ++i)
            lower << "\n[[widescreen.cull.sxy_x_lower_sites]]\n"
                  << "address = \"0x" << std::hex << kBase + 4u * i
                  << "\"\nexpected = \"0x" << std::hex
                  << (0x28000000u | (1u << 16)) << "\"\n";
        if (count > 64) {
            check(load_throws_with(lower.str(),
                                  "sxy_x_lower_sites supports at most 64 sites"),
                  "loader rejects shifted SXY sites beyond runtime capacity");
        } else {
            const auto path = write_temp_config("sxy-lower-boundary", lower.str());
            const auto config = PSXRecompV4::load_game_config(path);
            check(config.ws_cull_sxy_x_lower_sites.size() == count,
                  "loader preserves all 64 shifted SXY lower sites");
            fs::remove(path);
        }
    }
}

void codegen_overlay_sxy_final_mismatch_continues() {
    const auto body = base_config() + sxy_site(kBase, 0x00850824u) +
        sxy_site(kBase + 0x100u, 0x00280824u,
                 "fold_addresses = [\"0xA0020000\"]\n"
                 "fold_expected = [\"0x00C70824\"]\n");
    const auto path = write_temp_config("sxy-overlay-overlap", body);
    const auto loaded = PSXRecompV4::load_game_config(path);
    fs::remove(path);
    PSXRecomp::CodeGenConfig config{};
    config.overlay_mode = true;
    config.ws_cull_sxy_sites = loaded.ws_cull_sxy_sites;
    const auto folded = generate_first_instruction(
        0x00C70824u, config, 0x80020000u).full_code;
    check(folded.find("ws SXY cull folded") != std::string::npos,
          "overlay final mismatch continues to another site's matching aliased fold");
    const auto final = generate_first_instruction(0x00850824u, config).full_code;
    check(final.find("cpu->gpr[1] = psx_ws_cull_sxy_tri(") != std::string::npos,
          "overlay matching SXY final still emits helper");
    const auto unmatched = generate_first_instruction(0x00C70825u, config).full_code;
    PSXRecomp::CodeGenConfig vanilla{};
    vanilla.overlay_mode = true;
    check(unmatched == generate_first_instruction(0x00C70825u, vanilla).full_code,
          "overlay with no matching SXY instruction preserves vanilla emission");
    check(load_throws_with(base_config() + sxy_site(kBase, 0x00850824u) +
                          sxy_site(0xA0010000u, 0x00C70824u),
                          "duplicate [widescreen.cull] sxy final_address"),
          "SXY final addresses must remain physically unique");
}

void shared_decls_include_screen_helper() {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    PSXRecomp::CodeGenerator generator(exe);
    std::vector<PSXRecomp::GeneratedFunction> functions;
    const auto decls = generator.build_shared_decls_header(functions);
    check(decls.find("psx_ws_screen_x_bound") != std::string::npos,
          "shared declarations include psx_ws_screen_x_bound");
}

}  // namespace

int main() {
    loader_accepts_lui_and_addiu();
    loader_rejects_bad_addiu_shapes();
    codegen_routes_bound_kinds();
    loader_infers_sxy_result_register();
    loader_bounds_sxy_sites_and_folds();
    codegen_overlay_sxy_final_mismatch_continues();
    shared_decls_include_screen_helper();

    if (failures != 0) {
        std::fprintf(stderr, "signed_x_bound_codegen_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::puts("PASS: signed_x_bound codegen");
    return 0;
}
