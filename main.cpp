#include "process/process.h"
#include "process/signatures.h"
#include "game/cdatabase.h"
#include "game/sql_inject.h"

extern "C" {
#include "include/sqlite3.h"
}

#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

namespace
{
    constexpr auto PROCESS_NAME = L"forzahorizon6.exe";

    namespace clr
    {
        constexpr const char* RESET   = "\033[0m";
        constexpr const char* BOLD    = "\033[1m";
        constexpr const char* DIM     = "\033[2m";
        constexpr const char* RED     = "\033[91m";
        constexpr const char* GREEN   = "\033[92m";
        constexpr const char* YELLOW  = "\033[93m";
        constexpr const char* BLUE    = "\033[94m";
        constexpr const char* MAGENTA = "\033[95m";
        constexpr const char* CYAN    = "\033[96m";
        constexpr const char* WHITE   = "\033[97m";
        constexpr const char* GRAY    = "\033[90m";
    }

    void enable_ansi()
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hOut, &mode);
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    void clear_screen()
    {
        std::cout << "\033[2J\033[1;1H" << std::flush;
    }

    void move_cursor(int row, int col)
    {
        std::cout << std::format("\033[{};{}H", row, col) << std::flush;
    }

    std::string get_exe_dir()
    {
        char buf[MAX_PATH]{};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::filesystem::path p(buf);
        return p.parent_path().string();
    }

    std::string escape_sql(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s)
        {
            if (c == '\'') out += "''";
            else out += c;
        }
        return out;
    }

    std::string cell_to_sql(const game::CellValue& val)
    {
        if (std::holds_alternative<std::monostate>(val)) return "NULL";
        if (std::holds_alternative<int64_t>(val)) return std::to_string(std::get<int64_t>(val));
        if (std::holds_alternative<double>(val)) return std::format("{}", std::get<double>(val));
        if (std::holds_alternative<std::string>(val)) return "'" + escape_sql(std::get<std::string>(val)) + "'";
        return "NULL";
    }

    int64_t query_int(HANDLE proc, const game::CDatabase& db, const std::string& sql, int64_t fallback = 0)
    {
        auto r = game::execute_sql(proc, db, sql);
        if (r.success && r.parsed && !r.parsed->rows.empty() &&
            std::holds_alternative<int64_t>(r.parsed->rows[0][0]))
            return std::get<int64_t>(r.parsed->rows[0][0]);
        return fallback;
    }

    bool backup_exists(HANDLE proc, const game::CDatabase& db, const std::string& name)
    {
        return query_int(proc, db,
            "SELECT count(*) FROM sqlite_master WHERE type='table' AND tbl_name='" + name + "'") > 0;
    }

    bool run_sql(HANDLE proc, const game::CDatabase& db, const std::string& sql, const std::string& label)
    {
        auto r = game::execute_sql(proc, db, sql);
        if (!r.success)
        {
            std::cerr << clr::RED << "  [x] " << label << ": " << r.error << clr::RESET << "\n";
            return false;
        }
        return true;
    }

    int64_t get_garage_count(HANDLE proc, const game::CDatabase& db)
    {
        return query_int(proc, db, "SELECT count(*) FROM Profile0_Career_Garage");
    }

    // ============================================================
    // STATUS TRACKING
    // ============================================================

    struct StatusMessage
    {
        std::string text;
        bool success;
    };

    StatusMessage g_status = { "", true };

    void set_status(const std::string& msg, bool success = true)
    {
        g_status = { msg, success };
    }

    // ============================================================
    // TOGGLE STATE
    // ============================================================

    struct ToggleState
    {
        bool active;
        std::string detail;
    };

    ToggleState check_autoshow(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM Data_Car WHERE NotAvailableInAutoshow = 1");
        return { n == 0, n > 0 ? std::format("{} hidden", n) : "" };
    }

    ToggleState check_free_cars(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM Data_Car WHERE BaseCost > 0");
        return { n == 0, n > 0 ? std::format("{} priced", n) : "" };
    }

    ToggleState check_free_upgrades(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM List_UpgradeEngine WHERE price > 0");
        return { n == 0, n > 0 ? std::format("{} priced parts", n) : "" };
    }

    ToggleState check_hidden_cars(HANDLE proc, const game::CDatabase& db)
    {
        int64_t vis = query_int(proc, db, "SELECT count(*) FROM Data_Car WHERE VisibleOnlyIfOwned = 1");
        int64_t auc = query_int(proc, db, "SELECT count(*) FROM Data_Car WHERE NotAvailableInAuctionHouse = 1");
        bool on = (vis == 0 && auc == 0);
        return { on, on ? "" : std::format("{} hidden, {} no auction", vis, auc) };
    }

    ToggleState check_barn_finds(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM Profile0_BarnFinds WHERE State = 0");
        return { n == 0, n > 0 ? std::format("{} undiscovered", n) : "" };
    }

    ToggleState check_clear_new(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM Profile0_Career_Garage WHERE HasCurrentOwnerViewedCar = 0");
        return { n == 0, n > 0 ? std::format("{} unviewed", n) : "" };
    }

    ToggleState check_skill_points(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM Profile0_Career_Garage WHERE NumSkillPointsEarned < 999999");
        return { n == 0, n > 0 ? std::format("{} below 999K", n) : "" };
    }

    // ============================================================
    // APPLY / REVERT
    // ============================================================

    void apply_autoshow(HANDLE proc, const game::CDatabase& db)
    {
        if (!backup_exists(proc, db, "_backup_AutoshowState"))
            run_sql(proc, db, "CREATE TABLE _backup_AutoshowState AS SELECT Id, NotAvailableInAutoshow, BaseCost FROM Data_Car", "");
        run_sql(proc, db, "UPDATE Data_Car SET NotAvailableInAutoshow = 0", "");
        run_sql(proc, db, "DROP VIEW IF EXISTS Drivable_Data_Car", "");
        run_sql(proc, db, "CREATE VIEW Drivable_Data_Car AS SELECT * FROM Data_Car", "");
        if (!backup_exists(proc, db, "_backup_CarBuckets"))
            run_sql(proc, db, "CREATE TABLE _backup_CarBuckets AS SELECT * FROM CarBuckets", "");
        run_sql(proc, db, "INSERT OR IGNORE INTO CarBuckets(CarId) SELECT Id FROM Data_Car WHERE Id NOT IN (SELECT CarId FROM CarBuckets)", "");
        run_sql(proc, db, "UPDATE CarBuckets SET CarBucket=0, BucketHero=0 WHERE CarBucket IS NULL", "");
        set_status("All Cars in Autoshow applied");
    }

    void revert_autoshow(HANDLE proc, const game::CDatabase& db)
    {
        bool has1 = backup_exists(proc, db, "_backup_AutoshowState");
        bool has2 = backup_exists(proc, db, "_backup_HiddenCars");
        if (!has1 && !has2) { set_status("No backup found", false); return; }
        const char* tbl = has1 ? "_backup_AutoshowState" : "_backup_HiddenCars";
        run_sql(proc, db, std::string("UPDATE Data_Car SET NotAvailableInAutoshow = (SELECT b.NotAvailableInAutoshow FROM ") + tbl + " b WHERE b.Id = Data_Car.Id)", "");
        run_sql(proc, db, "DROP VIEW IF EXISTS Drivable_Data_Car", "");
        run_sql(proc, db, "CREATE VIEW Drivable_Data_Car AS SELECT * FROM Data_Car WHERE NotAvailableInAutoshow = 0", "");
        if (backup_exists(proc, db, "_backup_CarBuckets"))
        {
            run_sql(proc, db, "DELETE FROM CarBuckets", "");
            run_sql(proc, db, "INSERT INTO CarBuckets SELECT * FROM _backup_CarBuckets", "");
        }
        set_status("All Cars in Autoshow reverted");
    }

    void apply_free_cars(HANDLE proc, const game::CDatabase& db)
    {
        if (!backup_exists(proc, db, "_backup_CarPrices"))
            run_sql(proc, db, "CREATE TABLE _backup_CarPrices AS SELECT Id, BaseCost FROM Data_Car", "");
        run_sql(proc, db, "UPDATE Data_Car SET BaseCost = 0", "");
        set_status("Free Cars applied -- all prices set to 0");
    }

    void revert_free_cars(HANDLE proc, const game::CDatabase& db)
    {
        if (!backup_exists(proc, db, "_backup_CarPrices")) { set_status("No backup found", false); return; }
        run_sql(proc, db, "UPDATE Data_Car SET BaseCost = (SELECT b.BaseCost FROM _backup_CarPrices b WHERE b.Id = Data_Car.Id)", "");
        set_status("Free Cars reverted -- original prices restored");
    }

    void apply_free_upgrades(HANDLE proc, const game::CDatabase& db)
    {
        const char* tables[] = {
            "List_UpgradeAntiSwayFront", "List_UpgradeAntiSwayRear",
            "List_UpgradeBrakes", "List_UpgradeCarBodyChassisStiffness",
            "List_UpgradeCarBody", "List_UpgradeCarBodyTireAspectRatioFront",
            "List_UpgradeCarBodyTireAspectRatioRear", "List_UpgradeCarBodyTireWidthFront",
            "List_UpgradeCarBodyTireWidthRear", "List_UpgradeCarBodyTrackSpacingFront",
            "List_UpgradeCarBodyTrackSpacingRear", "List_UpgradeCarBodyWeight",
            "List_UpgradeDrivetrain", "List_UpgradeDrivetrainClutch",
            "List_UpgradeDrivetrainDifferential", "List_UpgradeDrivetrainDriveline",
            "List_UpgradeDrivetrainTransmission", "List_UpgradeEngine",
            "List_UpgradeEngineCamshaft", "List_UpgradeEngineCSC",
            "List_UpgradeEngineDisplacement", "List_UpgradeEngineDSC",
            "List_UpgradeEngineExhaust", "List_UpgradeEngineFlywheel",
            "List_UpgradeEngineFuelSystem", "List_UpgradeEngineIgnition",
            "List_UpgradeEngineIntake", "List_UpgradeEngineIntercooler",
            "List_UpgradeEngineManifold", "List_UpgradeEngineOilCooling",
            "List_UpgradeEnginePistonsCompression", "List_UpgradeEngineRestrictorPlate",
            "List_UpgradeEngineTurboSingle", "List_UpgradeEngineTurboTwin",
            "List_UpgradeEngineValves", "List_UpgradeMotor", "List_UpgradeMotorParts",
            "List_UpgradeRimSizeFront", "List_UpgradeRimSizeRear",
            "List_UpgradeSpringDamper", "List_UpgradeTireCompound",
            "List_UpgradeCarBodyFrontBumper", "List_UpgradeCarBodyHood",
            "List_UpgradeCarBodyRearBumper", "List_UpgradeCarBodySideSkirt",
            "List_UpgradeRearWing",
        };
        int ok = 0;
        for (auto t : tables)
            if (run_sql(proc, db, std::string("UPDATE [") + t + "] SET price=0", "")) ++ok;
        run_sql(proc, db, "UPDATE List_Wheels SET price=1", "");
        run_sql(proc, db, "UPDATE UpgradePresetPackages SET Purchasable=1 WHERE Purchasable=0", "");
        set_status(std::format("Free Upgrades applied -- {} tables set to 0", ok));
    }

    void apply_hidden_cars(HANDLE proc, const game::CDatabase& db)
    {
        if (!backup_exists(proc, db, "_backup_HiddenCars"))
            run_sql(proc, db, "CREATE TABLE _backup_HiddenCars AS SELECT Id, NotAvailableInAutoshow, NotAvailableInAuctionHouse, VisibleOnlyIfOwned FROM Data_Car", "");
        run_sql(proc, db, "UPDATE Data_Car SET NotAvailableInAutoshow = 0", "");
        run_sql(proc, db, "UPDATE Data_Car SET VisibleOnlyIfOwned = 0", "");
        run_sql(proc, db, "UPDATE Data_Car SET NotAvailableInAuctionHouse = 0", "");
        run_sql(proc, db, "DROP VIEW IF EXISTS Drivable_Data_Car", "");
        run_sql(proc, db, "CREATE VIEW Drivable_Data_Car AS SELECT * FROM Data_Car", "");
        if (!backup_exists(proc, db, "_backup_CarBuckets"))
            run_sql(proc, db, "CREATE TABLE _backup_CarBuckets AS SELECT * FROM CarBuckets", "");
        run_sql(proc, db, "INSERT OR IGNORE INTO CarBuckets(CarId) SELECT Id FROM Data_Car WHERE Id NOT IN (SELECT CarId FROM CarBuckets)", "");
        run_sql(proc, db, "UPDATE CarBuckets SET CarBucket=0, BucketHero=0 WHERE CarBucket IS NULL", "");
        set_status("Unlock Hidden Cars applied -- all cars visible and tradeable");
    }

    void revert_hidden_cars(HANDLE proc, const game::CDatabase& db)
    {
        if (!backup_exists(proc, db, "_backup_HiddenCars")) { set_status("No backup found", false); return; }
        run_sql(proc, db, "UPDATE Data_Car SET NotAvailableInAutoshow = (SELECT b.NotAvailableInAutoshow FROM _backup_HiddenCars b WHERE b.Id = Data_Car.Id)", "");
        run_sql(proc, db, "UPDATE Data_Car SET VisibleOnlyIfOwned = (SELECT b.VisibleOnlyIfOwned FROM _backup_HiddenCars b WHERE b.Id = Data_Car.Id)", "");
        run_sql(proc, db, "UPDATE Data_Car SET NotAvailableInAuctionHouse = (SELECT b.NotAvailableInAuctionHouse FROM _backup_HiddenCars b WHERE b.Id = Data_Car.Id)", "");
        run_sql(proc, db, "DROP VIEW IF EXISTS Drivable_Data_Car", "");
        run_sql(proc, db, "CREATE VIEW Drivable_Data_Car AS SELECT * FROM Data_Car WHERE NotAvailableInAutoshow = 0", "");
        if (backup_exists(proc, db, "_backup_CarBuckets"))
        {
            run_sql(proc, db, "DELETE FROM CarBuckets", "");
            run_sql(proc, db, "INSERT INTO CarBuckets SELECT * FROM _backup_CarBuckets", "");
        }
        set_status("Unlock Hidden Cars reverted");
    }

    void apply_barn_finds(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM Profile0_BarnFinds WHERE State = 0");
        run_sql(proc, db, "UPDATE Profile0_BarnFinds SET State = 1 WHERE State = 0", "");
        set_status(std::format("Barn Finds -- {} newly revealed", n));
    }

    void apply_clear_new(HANDLE proc, const game::CDatabase& db)
    {
        run_sql(proc, db, "UPDATE Profile0_Career_Garage SET HasCurrentOwnerViewedCar = 1", "");
        set_status("New Tags cleared on all garage cars");
    }

    void apply_skill_points(HANDLE proc, const game::CDatabase& db)
    {
        int64_t n = query_int(proc, db, "SELECT count(*) FROM Profile0_Career_Garage WHERE NumSkillPointsEarned < 999999");
        run_sql(proc, db, "UPDATE Profile0_Career_Garage SET NumSkillPointsEarned = 999999 WHERE NumSkillPointsEarned < 999999", "");
        set_status(std::format("Skill Points -- {} cars boosted to 999K earned", n));
    }

    // ============================================================
    // DUMP
    // ============================================================

    struct TableInfo { std::string name; std::string sql; };
    std::vector<TableInfo> get_tables(HANDLE proc, const game::CDatabase& db)
    {
        std::vector<TableInfo> tables;
        auto result = game::execute_sql(proc, db,
            "SELECT tbl_name, sql FROM sqlite_master WHERE type='table' ORDER BY tbl_name");
        if (!result.success || !result.parsed) return tables;
        for (const auto& row : result.parsed->rows)
            if (row.size() >= 2 && std::holds_alternative<std::string>(row[0]) && std::holds_alternative<std::string>(row[1]))
                tables.push_back({ std::get<std::string>(row[0]), std::get<std::string>(row[1]) });
        return tables;
    }

    struct IndexInfo { std::string sql; };
    std::vector<IndexInfo> get_indexes(HANDLE proc, const game::CDatabase& db)
    {
        std::vector<IndexInfo> indexes;
        auto result = game::execute_sql(proc, db,
            "SELECT sql FROM sqlite_master WHERE type='index' AND sql IS NOT NULL ORDER BY name");
        if (!result.success || !result.parsed) return indexes;
        for (const auto& row : result.parsed->rows)
            if (!row.empty() && std::holds_alternative<std::string>(row[0]))
                indexes.push_back({ std::get<std::string>(row[0]) });
        return indexes;
    }

    struct ViewInfo { std::string sql; };
    std::vector<ViewInfo> get_views(HANDLE proc, const game::CDatabase& db)
    {
        std::vector<ViewInfo> views;
        auto result = game::execute_sql(proc, db,
            "SELECT sql FROM sqlite_master WHERE type='view' AND sql IS NOT NULL ORDER BY name");
        if (!result.success || !result.parsed) return views;
        for (const auto& row : result.parsed->rows)
            if (!row.empty() && std::holds_alternative<std::string>(row[0]))
                views.push_back({ std::get<std::string>(row[0]) });
        return views;
    }

    void do_dump(HANDLE proc, const game::CDatabase& db, const std::string& path)
    {
        if (std::filesystem::exists(path)) std::filesystem::remove(path);

        sqlite3* local_db = nullptr;
        if (sqlite3_open(path.c_str(), &local_db) != SQLITE_OK)
        {
            set_status("Failed to create output file", false);
            sqlite3_close(local_db);
            return;
        }

        auto exec_local = [&](const std::string& sql) -> bool {
            char* err = nullptr;
            int rc = sqlite3_exec(local_db, sql.c_str(), nullptr, nullptr, &err);
            if (rc != SQLITE_OK) { sqlite3_free(err); return false; }
            return true;
        };

        exec_local("PRAGMA journal_mode=WAL");
        exec_local("PRAGMA synchronous=NORMAL");

        auto tables = get_tables(proc, db);
        auto indexes = get_indexes(proc, db);
        auto views = get_views(proc, db);

        exec_local("BEGIN TRANSACTION");
        for (const auto& table : tables) exec_local(table.sql + ";");

        size_t total_rows = 0;
        for (size_t t = 0; t < tables.size(); ++t)
        {
            const auto& table = tables[t];
            auto count_result = game::execute_sql(proc, db, "SELECT count(*) FROM [" + table.name + "]");
            int64_t row_count = 0;
            if (count_result.success && count_result.parsed && !count_result.parsed->rows.empty() &&
                std::holds_alternative<int64_t>(count_result.parsed->rows[0][0]))
                row_count = std::get<int64_t>(count_result.parsed->rows[0][0]);

            if (row_count == 0) continue;

            constexpr int64_t BATCH_SIZE = 500;
            int64_t rows_dumped = 0;
            for (int64_t offset = 0; offset < row_count; offset += BATCH_SIZE)
            {
                auto data = game::execute_sql(proc, db,
                    std::format("SELECT * FROM [{}] LIMIT {} OFFSET {}", table.name, BATCH_SIZE, offset));
                if (!data.success || !data.parsed || data.parsed->rows.empty()) break;
                for (const auto& row : data.parsed->rows)
                {
                    std::string insert = "INSERT INTO [" + table.name + "] VALUES(";
                    for (size_t c = 0; c < row.size(); ++c)
                    {
                        if (c > 0) insert += ",";
                        insert += cell_to_sql(row[c]);
                    }
                    insert += ")";
                    exec_local(insert);
                    ++rows_dumped;
                }
            }
            total_rows += static_cast<size_t>(rows_dumped);

            int pct = static_cast<int>((t + 1) * 100 / tables.size());
            int bar = pct / 5;
            std::cout << "\r  " << clr::CYAN << "[" << clr::RESET;
            for (int i = 0; i < 20; i++)
                std::cout << (i < bar ? std::string(clr::GREEN) + "#" + clr::RESET : std::string(clr::GRAY) + "-" + clr::RESET);
            std::cout << clr::CYAN << "] " << clr::RESET << pct << "% " << table.name << "        " << std::flush;
        }
        std::cout << "\n";

        for (const auto& idx : indexes) exec_local(idx.sql + ";");
        for (const auto& view : views) exec_local(view.sql + ";");
        exec_local("COMMIT");
        sqlite3_close(local_db);

        set_status(std::format("Dumped {} rows across {} tables to {}", total_rows, tables.size(), path));
    }

    // ============================================================
    // PERSIST
    // ============================================================

    void do_persist(HANDLE proc, const game::CDatabase& db, const std::string& input_path, int64_t max_rows)
    {
        if (!std::filesystem::exists(input_path))
        {
            set_status("File not found: " + input_path, false);
            return;
        }

        sqlite3* local_db = nullptr;
        if (sqlite3_open_v2(input_path.c_str(), &local_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        {
            set_status("Failed to open file", false);
            sqlite3_close(local_db);
            return;
        }

        struct LocalTable { std::string name; int64_t row_count; };
        std::vector<LocalTable> local_tables;
        {
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(local_db, "SELECT tbl_name FROM sqlite_master WHERE type='table' ORDER BY tbl_name", -1, &stmt, nullptr);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                sqlite3_stmt* cnt = nullptr;
                std::string sql = "SELECT count(*) FROM [" + name + "]";
                sqlite3_prepare_v2(local_db, sql.c_str(), -1, &cnt, nullptr);
                int64_t count = 0;
                if (sqlite3_step(cnt) == SQLITE_ROW) count = sqlite3_column_int64(cnt, 0);
                sqlite3_finalize(cnt);
                local_tables.push_back({ name, count });
            }
            sqlite3_finalize(stmt);
        }

        auto game_tables_result = game::execute_sql(proc, db, "SELECT tbl_name FROM sqlite_master WHERE type='table'");
        std::vector<std::string> game_tables;
        if (game_tables_result.success && game_tables_result.parsed)
            for (const auto& row : game_tables_result.parsed->rows)
                if (!row.empty() && std::holds_alternative<std::string>(row[0]))
                    game_tables.push_back(std::get<std::string>(row[0]));

        size_t total_rows = 0;
        for (size_t t = 0; t < local_tables.size(); ++t)
        {
            const auto& table = local_tables[t].name;
            int64_t row_count = local_tables[t].row_count;
            if (std::find(game_tables.begin(), game_tables.end(), table) == game_tables.end()) continue;
            if (max_rows > 0 && row_count > max_rows) continue;

            game::execute_sql(proc, db, "DELETE FROM [" + table + "]");

            sqlite3_stmt* data_stmt = nullptr;
            std::string select = "SELECT * FROM [" + table + "]";
            sqlite3_prepare_v2(local_db, select.c_str(), -1, &data_stmt, nullptr);

            size_t rows_inserted = 0;
            while (sqlite3_step(data_stmt) == SQLITE_ROW)
            {
                int ncols = sqlite3_column_count(data_stmt);
                std::string insert = "INSERT INTO [" + table + "] VALUES(";
                for (int c = 0; c < ncols; ++c)
                {
                    if (c > 0) insert += ",";
                    switch (sqlite3_column_type(data_stmt, c))
                    {
                    case SQLITE_NULL: insert += "NULL"; break;
                    case SQLITE_INTEGER: insert += std::to_string(sqlite3_column_int64(data_stmt, c)); break;
                    case SQLITE_FLOAT: insert += std::format("{}", sqlite3_column_double(data_stmt, c)); break;
                    case SQLITE_TEXT: {
                        auto text = reinterpret_cast<const char*>(sqlite3_column_text(data_stmt, c));
                        insert += "'" + escape_sql(text ? text : "") + "'"; break;
                    }
                    case SQLITE_BLOB: {
                        auto blob = static_cast<const uint8_t*>(sqlite3_column_blob(data_stmt, c));
                        int blobsz = sqlite3_column_bytes(data_stmt, c);
                        insert += "X'";
                        for (int b = 0; b < blobsz; b++) insert += std::format("{:02X}", blob[b]);
                        insert += "'"; break;
                    }
                    }
                }
                insert += ")";
                if (game::execute_sql(proc, db, insert).success) ++rows_inserted;
            }
            sqlite3_finalize(data_stmt);
            total_rows += rows_inserted;
        }
        sqlite3_close(local_db);

        set_status(std::format("Persisted {} rows into game", total_rows));
    }

    // ============================================================
    // GARAGE STATS
    // ============================================================

    void do_garage_stats(HANDLE proc, const game::CDatabase& db)
    {
        auto result = game::execute_sql(proc, db,
            "SELECT g.Id, d.Year, m.ManufacturerCode, d.MediaName, "
            "g.PerformanceIndex, g.ClassID, g.DistanceDriven, g.NumVictories, "
            "g.NumRaces, g.NumSkillPointsEarned, g.HighestSkillScore, "
            "g.OriginalOwner "
            "FROM Profile0_Career_Garage g "
            "JOIN Data_Car d ON g.CarId = d.Id "
            "LEFT JOIN List_CarMake m ON d.MakeID = m.ID "
            "ORDER BY g.PerformanceIndex DESC");

        if (!result.success || !result.parsed)
        {
            set_status("Query failed: " + result.error, false);
            return;
        }

        auto cell_str = [](const game::CellValue& v) -> std::string {
            if (std::holds_alternative<std::monostate>(v)) return "-";
            if (std::holds_alternative<int64_t>(v)) return std::to_string(std::get<int64_t>(v));
            if (std::holds_alternative<double>(v)) return std::format("{:.3f}", std::get<double>(v));
            if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
            return "?";
        };
        auto cell_dbl = [](const game::CellValue& v) -> double {
            if (std::holds_alternative<double>(v)) return std::get<double>(v);
            if (std::holds_alternative<int64_t>(v)) return static_cast<double>(std::get<int64_t>(v));
            return 0.0;
        };
        auto cell_i64 = [](const game::CellValue& v) -> int64_t {
            if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
            if (std::holds_alternative<double>(v)) return static_cast<int64_t>(std::get<double>(v));
            return 0;
        };

        const char* class_names[] = { "D", "C", "B", "A", "S1", "S2", "R", "X" };

        clear_screen();
        std::cout << "\n";
        std::cout << clr::BOLD << clr::CYAN << "  GARAGE" << clr::RESET << "\n\n";
        std::cout << clr::BOLD << clr::WHITE
                  << "  #  Car                          Year  Cls  PI    Distance     W    R   Skills\n"
                  << "  " << std::string(85, '-') << clr::RESET << "\n";

        int n = 0;
        for (const auto& row : result.parsed->rows)
        {
            if (row.size() < 12) continue;
            ++n;

            int class_id = static_cast<int>(cell_i64(row[5]));
            const char* cls = (class_id >= 0 && class_id <= 7) ? class_names[class_id] : "?";
            int pi_display = static_cast<int>(cell_dbl(row[4]) * 1000.0 + 0.5);
            int64_t dist = cell_i64(row[6]);

            std::string dist_str;
            if (dist > 1000000) dist_str = std::format("{:.1f}M", dist / 1000000.0);
            else if (dist > 1000) dist_str = std::format("{:.1f}K", dist / 1000.0);
            else dist_str = std::format("{}", dist);

            const char* cls_clr = clr::WHITE;
            if (class_id == 7) cls_clr = clr::RED;
            else if (class_id == 6) cls_clr = clr::MAGENTA;
            else if (class_id == 5) cls_clr = clr::YELLOW;
            else if (class_id == 4) cls_clr = clr::CYAN;
            else if (class_id == 3) cls_clr = clr::GREEN;

            std::string name = cell_str(row[2]) + " " + cell_str(row[3]);
            if (name.length() > 30) name = name.substr(0, 27) + "...";

            std::cout << std::format("  {:2} {}{:<30}{} {:4}  {}{:<3}{} {:4}  {:>8}  {:3} {:4} {:>8}\n",
                n, clr::BOLD, name, clr::RESET,
                cell_str(row[1]),
                cls_clr, cls, clr::RESET,
                pi_display, dist_str,
                cell_i64(row[7]), cell_i64(row[8]), cell_i64(row[9]));
        }

        std::cout << "\n" << clr::CYAN << "  " << result.parsed->rows.size() << " cars" << clr::RESET << "\n\n";
        std::cout << clr::DIM << "  Press Enter to return..." << clr::RESET;
        std::string dummy;
        std::getline(std::cin, dummy);
    }

    // ============================================================
    // EXPORT JSON
    // ============================================================

    void do_export_json(HANDLE proc, const game::CDatabase& db, const std::string& path)
    {
        auto result = game::execute_sql(proc, db, "SELECT * FROM Profile0_Career_Garage");
        if (!result.success || !result.parsed)
        {
            set_status("Query failed: " + result.error, false);
            return;
        }

        std::ofstream out(path);
        if (!out.is_open())
        {
            set_status("Cannot create file: " + path, false);
            return;
        }

        auto json_escape = [](const std::string& s) -> std::string {
            std::string o;
            for (char c : s)
            {
                switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default: o += c;
                }
            }
            return o;
        };

        out << "[\n";
        for (size_t r = 0; r < result.parsed->rows.size(); ++r)
        {
            out << "  {\n";
            for (size_t c = 0; c < result.parsed->columns.size() && c < result.parsed->rows[r].size(); ++c)
            {
                const auto& col = result.parsed->columns[c];
                const auto& val = result.parsed->rows[r][c];
                out << "    \"" << json_escape(col.name) << "\": ";
                if (std::holds_alternative<std::monostate>(val)) out << "null";
                else if (std::holds_alternative<int64_t>(val)) out << std::get<int64_t>(val);
                else if (std::holds_alternative<double>(val)) out << std::format("{}", std::get<double>(val));
                else if (std::holds_alternative<std::string>(val)) out << "\"" << json_escape(std::get<std::string>(val)) << "\"";
                if (c < result.parsed->columns.size() - 1) out << ",";
                out << "\n";
            }
            out << "  }";
            if (r < result.parsed->rows.size() - 1) out << ",";
            out << "\n";
        }
        out << "]\n";
        out.close();

        set_status(std::format("Exported {} cars to {}", result.parsed->rows.size(), path));
    }

    // ============================================================
    // RENDER
    // ============================================================

    struct ToggleEntry {
        int id;
        const char* label;
        const char* desc;
        ToggleState (*check)(HANDLE, const game::CDatabase&);
        void (*apply)(HANDLE, const game::CDatabase&);
        void (*revert)(HANDLE, const game::CDatabase&);
        const char* backup_name;
    };

    ToggleEntry TOGGLES[] = {
        { 1, "Autoshow",       "All Cars in Autoshow",   check_autoshow,     apply_autoshow,     revert_autoshow,     "_backup_AutoshowState" },
        { 2, "Free Cars",      "Prices to 0 CR",         check_free_cars,    apply_free_cars,    revert_free_cars,    "_backup_CarPrices" },
        { 3, "Free Upgrades",  "All upgrade parts free",  check_free_upgrades,apply_free_upgrades,nullptr,             nullptr },
        { 4, "Hidden Cars",    "Unlock all hidden cars",  check_hidden_cars,  apply_hidden_cars,  revert_hidden_cars,  "_backup_HiddenCars" },
        { 5, "Barn Finds",     "Reveal all barn finds",   check_barn_finds,   apply_barn_finds,   nullptr,             nullptr },
        { 6, "New Tags",       "Clear new car tags",      check_clear_new,    apply_clear_new,    nullptr,             nullptr },
    };

    constexpr int NUM_TOGGLES = sizeof(TOGGLES) / sizeof(TOGGLES[0]);

    // Cached toggle states (lazy refresh)
    ToggleState g_toggle_cache[NUM_TOGGLES] = {};
    bool g_toggle_dirty[NUM_TOGGLES] = {};

    void refresh_all_toggles(HANDLE proc, const game::CDatabase& db)
    {
        for (int i = 0; i < NUM_TOGGLES; ++i)
        {
            g_toggle_cache[i] = TOGGLES[i].check(proc, db);
            g_toggle_dirty[i] = false;
        }
    }

    void refresh_one_toggle(HANDLE proc, const game::CDatabase& db, int idx)
    {
        g_toggle_cache[idx] = TOGGLES[idx].check(proc, db);
        g_toggle_dirty[idx] = false;
    }

    int count_active_mods()
    {
        int n = 0;
        for (int i = 0; i < NUM_TOGGLES; ++i)
            if (g_toggle_cache[i].active) ++n;
        return n;
    }

    void render_menu(HANDLE hproc, const game::CDatabase& db, int64_t garage_count, DWORD pid)
    {
        clear_screen();

        // Header
        int active = count_active_mods();
        std::cout << "\n"
                  << clr::BOLD << clr::CYAN
                  << "  FH6 Database Dumper"
                  << clr::RESET << clr::DIM
                  << "  |  PID " << pid
                  << "  |  " << garage_count << " cars"
                  << "  |  " << active << "/" << NUM_TOGGLES << " mods active"
                  << clr::RESET << "\n";

        // Status message
        if (!g_status.text.empty())
        {
            const char* s_clr = g_status.success ? clr::GREEN : clr::RED;
            const char* icon = g_status.success ? "+" : "!";
            std::cout << "  " << s_clr << icon << " " << g_status.text << clr::RESET << "\n";
        }

        std::cout << clr::DIM << "  " << std::string(60, '-') << clr::RESET << "\n";

        // Toggles with numbers
        for (int i = 0; i < NUM_TOGGLES; ++i)
        {
            const auto& t = TOGGLES[i];
            const auto& st = g_toggle_cache[i];

            if (st.active)
            {
                std::cout << std::format("  {}[{}]{} {} {}{}{}\n",
                    clr::BOLD, i + 1, clr::RESET,
                    clr::WHITE, t.label, clr::RESET,
                    std::string(clr::GREEN) + "  ON" + clr::RESET);
            }
            else
            {
                std::string detail = st.detail.empty() ? t.desc : st.detail;
                std::cout << std::format("  {}[{}]{} {} {}{}{}\n",
                    clr::BOLD, i + 1, clr::RESET,
                    clr::WHITE, t.label, clr::RESET,
                    std::string(clr::RED) + "  OFF" + clr::RESET + clr::DIM + "  " + detail + clr::RESET);
            }
        }

        std::cout << clr::DIM << "  " << std::string(60, '-') << clr::RESET << "\n";

        // Actions with letter keys
        std::cout << std::format("  {}[D]{}ump   {}[L]{}oad   {}[S]{}tats   {}[E]{}xport   {}[0]{} Exit\n",
            clr::BOLD, clr::RESET,
            clr::BOLD, clr::RESET,
            clr::BOLD, clr::RESET,
            clr::BOLD, clr::RESET,
            clr::BOLD, clr::RESET);

        std::cout << clr::DIM << "  " << std::string(60, '-') << clr::RESET << "\n";
        std::cout << clr::BOLD << "  > " << clr::RESET << std::flush;
    }

} // anonymous namespace

// ============================================================
// MAIN
// ============================================================

int main()
{
    enable_ansi();

    // Boot
    std::cout << "\n" << clr::DIM << "  Connecting..." << clr::RESET << "\n";

    auto proc = process::open_process(PROCESS_NAME);
    if (!proc)
    {
        std::cout << clr::RED << "  ! Game not found\n" << clr::RESET;
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }
    std::cout << clr::GREEN << "  + Process found (PID " << proc->pid << ")" << clr::RESET << "\n";

    auto db = game::resolve_cdatabase(proc->handle, proc->base_address, proc->image_size);
    if (!db)
    {
        std::cout << clr::RED << "  ! CDatabase not found\n" << clr::RESET;
        process::close_process(*proc);
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }
    std::cout << clr::GREEN << "  + CDatabase resolved" << clr::RESET << "\n";

    auto test = game::execute_sql(proc->handle, *db, "SELECT count(*) FROM sqlite_master");
    if (!test.success || !test.parsed)
    {
        std::cout << clr::RED << "  ! SQL failed: " << test.error << clr::RESET << "\n";
        process::close_process(*proc);
        std::cout << "  Press Enter to exit...";
        std::cin.get();
        return 1;
    }
    std::cout << clr::GREEN << "  + Database online" << clr::RESET << "\n";

    std::string db_path = get_exe_dir() + "\\fh6_db.sqlite";
    std::string json_path = get_exe_dir() + "\\fh6_garage.json";

    // Initial toggle state fetch
    refresh_all_toggles(proc->handle, *db);

    // Main loop
    while (true)
    {
        int64_t garage_count = get_garage_count(proc->handle, *db);
        render_menu(proc->handle, *db, garage_count, proc->pid);

        std::string input;
        std::getline(std::cin, input);

        if (input.empty()) continue;

        if (input == "0" || input == "q" || input == "Q")
            break;

        // Toggle mods (1-7)
        int num = -1;
        try { num = std::stoi(input); } catch (...) {}

        if (num >= 1 && num <= NUM_TOGGLES)
        {
            int idx = num - 1;
            const auto& t = TOGGLES[idx];
            const auto& st = g_toggle_cache[idx];

            if (st.active && t.revert)
            {
                bool has_backup = t.backup_name && backup_exists(proc->handle, *db, t.backup_name);
                if (!has_backup && idx == 0) // autoshow fallback
                    has_backup = backup_exists(proc->handle, *db, "_backup_HiddenCars");

                if (has_backup)
                {
                    // Confirmation
                    std::cout << clr::YELLOW << "  Revert " << t.label << "? [y/N]: " << clr::RESET << std::flush;
                    std::string confirm;
                    std::getline(std::cin, confirm);
                    if (!confirm.empty() && (confirm[0] == 'y' || confirm[0] == 'Y'))
                    {
                        t.revert(proc->handle, *db);
                    }
                    else
                    {
                        set_status("Cancelled");
                    }
                }
                else
                {
                    set_status("No backup to revert -- reboot game to reset", false);
                }
            }
            else if (!st.active)
            {
                t.apply(proc->handle, *db);
            }
            else
            {
                set_status("Already active, no revert available", false);
            }

            // Lazy refresh: only re-query the toggled item
            refresh_one_toggle(proc->handle, *db, idx);
            continue;
        }

        // Actions
        char cmd = std::toupper(input[0]);

        if (cmd == 'D')
        {
            std::cout << "  File [" << clr::BOLD << "fh6_db.sqlite" << clr::RESET << "]: " << std::flush;
            std::string name;
            std::getline(std::cin, name);
            if (!name.empty()) db_path = get_exe_dir() + "\\" + name;
            do_dump(proc->handle, *db, db_path);
        }
        else if (cmd == 'L')
        {
            if (!std::filesystem::exists(db_path))
            {
                set_status("No dump file found -- run Dump first", false);
                continue;
            }
            std::cout << clr::YELLOW << "  Overwrite game data? [y/N]: " << clr::RESET << std::flush;
            std::string confirm;
            std::getline(std::cin, confirm);
            if (!confirm.empty() && (confirm[0] == 'y' || confirm[0] == 'Y'))
            {
                std::cout << "  Max rows/table [500, 0=unlimited]: " << std::flush;
                std::string rows_input;
                std::getline(std::cin, rows_input);
                int64_t max_rows = 500;
                try { max_rows = std::stoll(rows_input); } catch (...) {}
                if (max_rows < 0) max_rows = 0;
                do_persist(proc->handle, *db, db_path, max_rows);
            }
            else
            {
                set_status("Cancelled");
            }
        }
        else if (cmd == 'S')
        {
            do_garage_stats(proc->handle, *db);
        }
        else if (cmd == 'E')
        {
            std::cout << "  File [" << clr::BOLD << "fh6_garage.json" << clr::RESET << "]: " << std::flush;
            std::string name;
            std::getline(std::cin, name);
            if (!name.empty()) json_path = get_exe_dir() + "\\" + name;
            do_export_json(proc->handle, *db, json_path);
        }
        else
        {
            set_status("Unknown command: " + input, false);
        }
    }

    process::close_process(*proc);
    clear_screen();
    std::cout << "\n" << clr::DIM << "  Goodbye.\n" << clr::RESET;
    return 0;
}
