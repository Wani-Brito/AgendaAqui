#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
std::mutex database_mutex;
constexpr ULONG password_hash_iterations = 600000;
constexpr ULONG session_lifetime_seconds = 60 * 60 * 24 * 7;
bool legacy_password_hash_column = false;

struct AuthenticatedUser {
    sqlite3_int64 id = 0;
    std::string username;
    std::string role;

    bool isAdmin() const {
        return role == "admin";
    }
};

struct LoginAccount {
    AuthenticatedUser user;
    std::string password;
    std::string legacy_password_hash;
};

void checkCrypto(NTSTATUS status, const char* operation) {
    if (status < 0) {
        throw std::runtime_error(std::string("Falha na operação criptográfica: ") + operation);
    }
}

class AlgorithmHandle {
public:
    explicit AlgorithmHandle(LPCWSTR algorithm, ULONG flags = 0) {
        checkCrypto(
            BCryptOpenAlgorithmProvider(&handle_, algorithm, nullptr, flags),
            "abrir o algoritmo");
    }

    ~AlgorithmHandle() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    BCRYPT_ALG_HANDLE get() const {
        return handle_;
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class HashHandle {
public:
    explicit HashHandle(BCRYPT_ALG_HANDLE algorithm) {
        checkCrypto(BCryptCreateHash(algorithm, &handle_, nullptr, 0, nullptr, 0, 0), "criar hash");
    }

    ~HashHandle() {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }

    BCRYPT_HASH_HANDLE get() const {
        return handle_;
    }

    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

std::string toHex(const std::vector<unsigned char>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

std::vector<unsigned char> fromHex(const std::string& value) {
    if (value.size() % 2 != 0) {
        throw std::invalid_argument("Hash de senha inválido.");
    }
    const auto nibble = [](char character) -> unsigned char {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned char>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<unsigned char>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F') {
            return static_cast<unsigned char>(character - 'A' + 10);
        }
        throw std::invalid_argument("Hash de senha inválido.");
    };
    std::vector<unsigned char> result(value.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<unsigned char>(
            (nibble(value[i * 2]) << 4) | nibble(value[i * 2 + 1]));
    }
    return result;
}

std::vector<unsigned char> randomBytes(ULONG length) {
    std::vector<unsigned char> bytes(length);
    checkCrypto(
        BCryptGenRandom(
            nullptr,
            bytes.data(),
            length,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG),
        "gerar dados aleatórios");
    return bytes;
}

bool constantTimeEqual(const std::vector<unsigned char>& left, const std::vector<unsigned char>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return difference == 0;
}

bool verifyPassword(const std::string& password, const std::string& encoded_hash) {
    const auto algorithm_separator = encoded_hash.find('$');
    const auto iterations_separator = encoded_hash.find('$', algorithm_separator + 1);
    const auto salt_separator = encoded_hash.find('$', iterations_separator + 1);
    if (algorithm_separator == std::string::npos ||
        iterations_separator == std::string::npos ||
        salt_separator == std::string::npos ||
        encoded_hash.substr(0, algorithm_separator) != "pbkdf2_sha256" ||
        encoded_hash.substr(algorithm_separator + 1, iterations_separator - algorithm_separator - 1) !=
            std::to_string(password_hash_iterations)) {
        return false;
    }

    const auto salt = fromHex(encoded_hash.substr(iterations_separator + 1, salt_separator - iterations_separator - 1));
    const auto expected = fromHex(encoded_hash.substr(salt_separator + 1));
    std::vector<unsigned char> derived_key(expected.size());
    AlgorithmHandle algorithm(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    checkCrypto(
        BCryptDeriveKeyPBKDF2(
            algorithm.get(),
            reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
            static_cast<ULONG>(password.size()),
            const_cast<PUCHAR>(salt.data()),
            static_cast<ULONG>(salt.size()),
            password_hash_iterations,
            derived_key.data(),
            static_cast<ULONG>(derived_key.size()),
            0),
        "verificar a senha");
    return constantTimeEqual(derived_key, expected);
}

std::string hashSessionToken(const std::string& token) {
    AlgorithmHandle algorithm(BCRYPT_SHA256_ALGORITHM);
    HashHandle hash(algorithm.get());
    std::array<unsigned char, 32> digest{};
    checkCrypto(
        BCryptHashData(
            hash.get(),
            reinterpret_cast<PUCHAR>(const_cast<char*>(token.data())),
            static_cast<ULONG>(token.size()),
            0),
        "proteger a sessão");
    checkCrypto(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0), "finalizar hash");
    return toHex(std::vector<unsigned char>(digest.begin(), digest.end()));
}

std::string createSessionToken() {
    return toHex(randomBytes(32));
}

std::string sessionTokenFromCookie(const std::string& cookie_header) {
    constexpr char cookie_name[] = "agenda_session=";
    std::size_t position = 0;
    while (position < cookie_header.size()) {
        while (position < cookie_header.size() &&
               (cookie_header[position] == ' ' || cookie_header[position] == ';')) {
            ++position;
        }
        const auto end = cookie_header.find(';', position);
        const auto length = (end == std::string::npos ? cookie_header.size() : end) - position;
        const auto cookie = cookie_header.substr(position, length);
        if (cookie.compare(0, sizeof(cookie_name) - 1, cookie_name) == 0) {
            const auto token = cookie.substr(sizeof(cookie_name) - 1);
            static const std::regex token_pattern(R"(^[0-9a-f]{64}$)");
            return std::regex_match(token, token_pattern) ? token : std::string();
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    return {};
}

struct DatabaseCloser {
    void operator()(sqlite3* database) const {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

std::unique_ptr<sqlite3, DatabaseCloser> database;

class Statement {
public:
    Statement(sqlite3* handle, const char* sql) {
        if (sqlite3_prepare_v2(handle, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(handle));
        }
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const {
        return statement_;
    }

    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database.get()));
        }
    }

    void bind(int index, sqlite3_int64 value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database.get()));
        }
    }

    void bindNull(int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database.get()));
        }
    }

private:
    sqlite3_stmt* statement_ = nullptr;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Não foi possível abrir " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void executeSql(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(database.get(), sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? sqlite3_errmsg(database.get()) : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

bool tableHasColumn(const std::string& table, const std::string& column) {
    Statement statement(database.get(), ("PRAGMA table_info(" + table + ")").c_str());
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto* name = sqlite3_column_text(statement.get(), 1);
        if (name != nullptr && column == reinterpret_cast<const char*>(name)) {
            return true;
        }
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return false;
}

void migrateDatabase() {
    if (!tableHasColumn("users", "role")) {
        executeSql("ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'user'");
    }
    if (!tableHasColumn("users", "active")) {
        executeSql("ALTER TABLE users ADD COLUMN active INTEGER NOT NULL DEFAULT 1");
    }
    if (!tableHasColumn("users", "password")) {
        executeSql("ALTER TABLE users ADD COLUMN password TEXT NOT NULL DEFAULT ''");
    }
    legacy_password_hash_column = tableHasColumn("users", "password_hash");
    if (!tableHasColumn("appointments", "owner_user_id")) {
        executeSql(
            "ALTER TABLE appointments ADD COLUMN owner_user_id INTEGER "
            "REFERENCES users(id) ON DELETE SET NULL");
    }
    if (!tableHasColumn("appointments", "availability_slot_id")) {
        executeSql(
            "ALTER TABLE appointments ADD COLUMN availability_slot_id INTEGER "
            "REFERENCES availability_slots(id) ON DELETE SET NULL");
    }
    if (!tableHasColumn("appointments", "payment_type")) {
        executeSql(
            "ALTER TABLE appointments ADD COLUMN payment_type TEXT NOT NULL DEFAULT 'particular'");
    }
    if (!tableHasColumn("appointments", "insurance_name")) {
        executeSql(
            "ALTER TABLE appointments ADD COLUMN insurance_name TEXT NOT NULL DEFAULT ''");
    }
    if (!tableHasColumn("appointments", "confirmed_at")) {
        executeSql("ALTER TABLE appointments ADD COLUMN confirmed_at TEXT");
    }
    if (!tableHasColumn("availability_slots", "specialty")) {
        executeSql(
            "ALTER TABLE availability_slots ADD COLUMN specialty TEXT NOT NULL "
            "DEFAULT 'Clínica geral'");
    }
    const bool has_legacy_doctor_name = tableHasColumn("availability_slots", "doctor_name");
    if (!tableHasColumn("availability_slots", "professional_name")) {
        executeSql(
            "ALTER TABLE availability_slots ADD COLUMN professional_name TEXT NOT NULL DEFAULT ''");
    }
    if (has_legacy_doctor_name) {
        executeSql(
            "UPDATE availability_slots SET professional_name = doctor_name "
            "WHERE professional_name = '' AND trim(doctor_name) != ''");
    }
    executeSql(
        "CREATE INDEX IF NOT EXISTS idx_appointments_owner_date_time "
        "ON appointments (owner_user_id, appointment_date, start_time, end_time)");
    executeSql(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_appointments_availability_slot "
        "ON appointments (availability_slot_id) WHERE availability_slot_id IS NOT NULL");
    executeSql(
        "CREATE INDEX IF NOT EXISTS idx_availability_slots_specialty_date "
        "ON availability_slots (specialty, appointment_date, start_time)");
}

std::string trimText(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void ensureDefaultAccounts() {
    const char* sql = legacy_password_hash_column
        ? "INSERT OR IGNORE INTO users (username, password, password_hash, role) "
          "VALUES (?, ?, '', ?)"
        : "INSERT OR IGNORE INTO users (username, password, role) VALUES (?, ?, ?)";
    for (const auto& account : {
             std::array<std::string, 3>{"admin", "adminpassword", "admin"},
             std::array<std::string, 3>{"usercomum", "usercompassword", "user"}}) {
        Statement statement(database.get(), sql);
        statement.bind(1, account[0]);
        statement.bind(2, account[1]);
        statement.bind(3, account[2]);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database.get()));
        }
    }
}

void promoteExistingAccountAndMigrateAppointments() {
    executeSql(
        "UPDATE users SET role = 'admin' "
        "WHERE id = (SELECT id FROM users ORDER BY id LIMIT 1) "
        "AND NOT EXISTS (SELECT 1 FROM users WHERE role = 'admin')");
    executeSql(
        "UPDATE appointments SET owner_user_id = "
        "(SELECT id FROM users WHERE role = 'admin' ORDER BY id LIMIT 1) "
        "WHERE owner_user_id IS NULL");
}

void initializeDatabase() {
    const char* configured_path = std::getenv("AGENDA_DB_PATH");
    const std::filesystem::path path =
        configured_path == nullptr ? std::filesystem::path("data/agendaaqui.db")
                                   : std::filesystem::path(configured_path);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    sqlite3* handle = nullptr;
    const int result = sqlite3_open_v2(
        path.string().c_str(),
        &handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK) {
        const std::string message =
            handle == nullptr ? "Não foi possível abrir o banco SQLite." : sqlite3_errmsg(handle);
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        throw std::runtime_error(message);
    }

    database.reset(handle);
    sqlite3_busy_timeout(database.get(), 5000);
    executeSql(readFile(std::filesystem::path(AGENDA_SOURCE_DIR) / "database" / "schema.sql"));
    migrateDatabase();
    ensureDefaultAccounts();
    promoteExistingAccountAndMigrateAppointments();
}

bool findLoginAccount(const std::string& username, LoginAccount& account) {
    std::lock_guard<std::mutex> lock(database_mutex);
    Statement statement(
        database.get(),
        legacy_password_hash_column
            ? "SELECT id, username, password, role, password_hash "
              "FROM users WHERE username = ? AND active = 1"
            : "SELECT id, username, password, role "
              "FROM users WHERE username = ? AND active = 1");
    statement.bind(1, username);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return false;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    const auto text = [statement = statement.get()](int column) {
        const auto* value = sqlite3_column_text(statement, column);
        return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
    };
    account.user.id = sqlite3_column_int64(statement.get(), 0);
    account.user.username = text(1);
    account.password = text(2);
    account.user.role = text(3);
    if (legacy_password_hash_column) {
        account.legacy_password_hash = text(4);
    }
    return true;
}

bool authenticatedUser(const httplib::Request& request, AuthenticatedUser& user) {
    const auto token = sessionTokenFromCookie(request.get_header_value("Cookie"));
    if (token.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(database_mutex);
    Statement statement(
        database.get(),
        "SELECT users.id, users.username, users.role FROM sessions "
        "JOIN users ON users.id = sessions.user_id "
        "WHERE sessions.token_hash = ? AND sessions.expires_at > strftime('%s', 'now') "
        "AND users.active = 1");
    statement.bind(1, hashSessionToken(token));
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
        return false;
    }
    if (result != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    const auto text = [statement = statement.get()](int column) {
        const auto* value = sqlite3_column_text(statement, column);
        return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
    };
    user.id = sqlite3_column_int64(statement.get(), 0);
    user.username = text(1);
    user.role = text(2);
    return true;
}

void setJson(httplib::Response& response, const Json& body, int status = 200);
void setError(httplib::Response& response, int status, const std::string& message);

bool requireAuthentication(
    const httplib::Request& request,
    httplib::Response& response,
    AuthenticatedUser& user) {
    if (authenticatedUser(request, user)) {
        return true;
    }
    setError(response, 401, "Faça login para acessar sua agenda.");
    response.set_header(
        "Set-Cookie",
        "agenda_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return false;
}

bool requireAdmin(const httplib::Request& request, httplib::Response& response, AuthenticatedUser& user) {
    if (!requireAuthentication(request, response, user)) {
        return false;
    }
    if (!user.isAdmin()) {
        setError(response, 403, "Acesso restrito ao administrador.");
        return false;
    }
    return true;
}

Json appointmentFromRow(sqlite3_stmt* statement) {
    const auto text = [statement](int column) {
        const auto* value = sqlite3_column_text(statement, column);
        return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
    };
    return {
        {"id", sqlite3_column_int64(statement, 0)},
        {"title", text(1)},
        {"client", text(2)},
        {"date", text(3)},
        {"startTime", text(4)},
        {"endTime", text(5)},
        {"notes", text(6)},
        {"ownerUsername", text(7)},
        {"availabilitySlotId", sqlite3_column_type(statement, 8) == SQLITE_NULL
             ? Json(nullptr)
             : Json(sqlite3_column_int64(statement, 8))},
        {"specialty", text(9)},
        {"professionalName", text(10)},
        {"paymentType", text(11)},
        {"insuranceName", text(12)},
        {"confirmedAt", sqlite3_column_type(statement, 13) == SQLITE_NULL
             ? Json(nullptr)
             : Json(text(13))}
    };
}

Json listAppointments(const AuthenticatedUser& user, bool admin_view = false) {
    Statement statement(
        database.get(),
        user.isAdmin() && admin_view
            ? "SELECT appointments.id, appointments.title, appointments.client, "
              "appointments.appointment_date, appointments.start_time, appointments.end_time, "
              "appointments.notes, "
              "COALESCE(users.username, 'Conta removida'), availability_slot_id, "
              "COALESCE(availability_slots.specialty, ''), "
              "COALESCE(availability_slots.professional_name, ''), "
              "appointments.payment_type, appointments.insurance_name, appointments.confirmed_at "
              "FROM appointments LEFT JOIN users ON users.id = appointments.owner_user_id "
              "LEFT JOIN availability_slots ON availability_slots.id = appointments.availability_slot_id "
              "ORDER BY appointments.appointment_date, appointments.start_time"
            : "SELECT appointments.id, appointments.title, appointments.client, "
              "appointments.appointment_date, appointments.start_time, appointments.end_time, "
              "appointments.notes, "
              "users.username, availability_slot_id, COALESCE(availability_slots.specialty, ''), "
              "COALESCE(availability_slots.professional_name, ''), "
              "appointments.payment_type, appointments.insurance_name, appointments.confirmed_at "
              "FROM appointments "
              "JOIN users ON users.id = appointments.owner_user_id "
              "LEFT JOIN availability_slots ON availability_slots.id = appointments.availability_slot_id "
              "WHERE appointments.owner_user_id = ? "
              "ORDER BY appointments.appointment_date, appointments.start_time");
    if (!(user.isAdmin() && admin_view)) {
        statement.bind(1, user.id);
    }
    Json appointments = Json::array();
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        appointments.push_back(appointmentFromRow(statement.get()));
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return appointments;
}

Json listNotifications(const AuthenticatedUser& user) {
    const bool admin_view = user.isAdmin();
    Statement statement(
        database.get(),
        admin_view
            ? "SELECT appointments.id, COALESCE(users.username, 'Conta removida'), "
              "appointments.appointment_date, appointments.start_time, "
              "COALESCE(availability_slots.specialty, appointments.title), "
              "COALESCE(availability_slots.professional_name, ''), "
              "appointments.payment_type, appointments.insurance_name, appointments.confirmed_at "
              "FROM appointments "
              "LEFT JOIN users ON users.id = appointments.owner_user_id "
              "LEFT JOIN availability_slots ON availability_slots.id = appointments.availability_slot_id "
              "WHERE appointments.confirmed_at IS NOT NULL "
              "AND datetime(appointments.confirmed_at) >= datetime('now', 'localtime', '-7 days') "
              "ORDER BY datetime(appointments.confirmed_at) DESC"
            : "SELECT appointments.id, COALESCE(users.username, ''), "
              "appointments.appointment_date, appointments.start_time, "
              "COALESCE(availability_slots.specialty, appointments.title), "
              "COALESCE(availability_slots.professional_name, ''), "
              "appointments.payment_type, appointments.insurance_name, appointments.confirmed_at "
              "FROM appointments "
              "LEFT JOIN users ON users.id = appointments.owner_user_id "
              "LEFT JOIN availability_slots ON availability_slots.id = appointments.availability_slot_id "
              "WHERE appointments.owner_user_id = ? "
              "AND datetime(appointments.appointment_date || ' ' || appointments.start_time) "
              "> datetime('now', 'localtime') "
              "AND datetime(appointments.appointment_date || ' ' || appointments.start_time) "
              "<= datetime('now', 'localtime', '+24 hours') "
              "ORDER BY appointments.appointment_date, appointments.start_time");
    if (!admin_view) {
        statement.bind(1, user.id);
    }

    const auto text = [&statement](int column) {
        const auto* value = sqlite3_column_text(statement.get(), column);
        return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
    };
    Json notifications = Json::array();
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        Json notification = {
            {"appointmentId", sqlite3_column_int64(statement.get(), 0)},
            {"date", text(2)},
            {"startTime", text(3)},
            {"specialty", text(4)},
            {"professionalName", text(5)},
            {"paymentType", text(6)},
            {"insuranceName", text(7)},
            {"status", sqlite3_column_type(statement.get(), 8) == SQLITE_NULL ? "pending" : "confirmed"},
            {"confirmedAt", sqlite3_column_type(statement.get(), 8) == SQLITE_NULL
                 ? Json(nullptr)
                 : Json(text(8))}
        };
        if (admin_view) {
            notification["patientUsername"] = text(1);
        }
        notifications.push_back(std::move(notification));
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return notifications;
}

Json availabilityFromRow(sqlite3_stmt* statement) {
    const auto text = [statement](int column) {
        const auto* value = sqlite3_column_text(statement, column);
        return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
    };
    return {
        {"id", sqlite3_column_int64(statement, 0)},
        {"date", text(1)},
        {"startTime", text(2)},
        {"endTime", text(3)},
        {"isBooked", sqlite3_column_int(statement, 4) != 0},
        {"specialty", text(5)},
        {"bookedBy", text(6)},
        {"professionalName", text(7)}
    };
}

Json listAvailability(bool admin_view) {
    Statement statement(
        database.get(),
        admin_view
            ? "SELECT availability_slots.id, availability_slots.appointment_date, "
              "availability_slots.start_time, availability_slots.end_time, "
              "appointments.id IS NOT NULL, availability_slots.specialty, "
              "COALESCE(users.username, ''), availability_slots.professional_name "
              "FROM availability_slots "
              "LEFT JOIN appointments ON appointments.availability_slot_id = availability_slots.id "
              "LEFT JOIN users ON users.id = appointments.owner_user_id "
              "ORDER BY availability_slots.appointment_date, availability_slots.start_time"
            : "SELECT availability_slots.id, appointment_date, start_time, end_time, 0, "
              "specialty, '', professional_name "
              "FROM availability_slots WHERE (appointment_date > date('now', 'localtime') "
              "OR (appointment_date = date('now', 'localtime') "
              "AND time(start_time) > time('now', 'localtime'))) "
              "AND NOT EXISTS (SELECT 1 FROM appointments "
              "WHERE appointments.availability_slot_id = availability_slots.id) "
              "ORDER BY appointment_date, start_time");
    Json slots = Json::array();
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
        slots.push_back(availabilityFromRow(statement.get()));
    }
    if (result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return slots;
}

bool slotIsAvailable(
    sqlite3_int64 slot_id,
    const std::string& date,
    const std::string& start,
    const std::string& end,
    sqlite3_int64 excluded_appointment_id = 0) {
    Statement statement(
        database.get(),
        "SELECT 1 FROM availability_slots "
        "WHERE id = ? AND appointment_date = ? AND start_time = ? AND end_time = ? "
        "AND (appointment_date > date('now', 'localtime') "
        "OR (appointment_date = date('now', 'localtime') "
        "AND time(start_time) > time('now', 'localtime'))) "
        "AND NOT EXISTS (SELECT 1 FROM appointments "
        "WHERE availability_slot_id = availability_slots.id AND id != ?) LIMIT 1");
    statement.bind(1, slot_id);
    statement.bind(2, date);
    statement.bind(3, start);
    statement.bind(4, end);
    statement.bind(5, excluded_appointment_id);
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return result == SQLITE_ROW;
}

bool hasAvailabilityConflict(
    const std::string& date,
    const std::string& start,
    const std::string& end,
    const std::string& specialty,
    const std::string& professional_name,
    sqlite3_int64 excluded_slot_id = 0) {
    Statement statement(
        database.get(),
        "SELECT 1 FROM availability_slots "
        "WHERE appointment_date = ? AND start_time < ? AND end_time > ? AND id != ? "
        "AND specialty = ? COLLATE NOCASE "
        "AND professional_name = ? COLLATE NOCASE LIMIT 1");
    statement.bind(1, date);
    statement.bind(2, end);
    statement.bind(3, start);
    statement.bind(4, excluded_slot_id);
    statement.bind(5, specialty);
    statement.bind(6, professional_name);
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return result == SQLITE_ROW;
}

bool isFutureAvailability(const std::string& date, const std::string& start) {
    Statement statement(
        database.get(),
        "SELECT ? > date('now', 'localtime') OR "
        "(? = date('now', 'localtime') AND time(?) > time('now', 'localtime'))");
    statement.bind(1, date);
    statement.bind(2, date);
    statement.bind(3, start);
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return sqlite3_column_int(statement.get(), 0) != 0;
}

bool isValidDate(const std::string& value) {
    static const std::regex pattern(R"(^\d{4}-\d{2}-\d{2}$)");
    if (!std::regex_match(value, pattern)) {
        return false;
    }

    const int year = std::stoi(value.substr(0, 4));
    const int month = std::stoi(value.substr(5, 2));
    const int day = std::stoi(value.substr(8, 2));
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return false;
    }
    const bool leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int maximum_day = days_in_month[month - 1] + (month == 2 && leap_year ? 1 : 0);
    return day >= 1 && day <= maximum_day;
}

bool isValidTime(const std::string& value) {
    static const std::regex pattern(R"(^([01]\d|2[0-3]):[0-5]\d$)");
    return std::regex_match(value, pattern);
}

bool readAppointment(
    const Json& body,
    const AuthenticatedUser& user,
    Json& appointment,
    std::string& error) {
    if (!body.is_object()) {
        error = "Envie os dados do compromisso em formato JSON.";
        return false;
    }
    for (const auto* field : {"date", "startTime", "endTime"}) {
        if (!body.contains(field) || !body[field].is_string()) {
            error = std::string("O campo ") + field + " é obrigatório.";
            return false;
        }
    }
    if (user.isAdmin() &&
        (!body.contains("title") || !body["title"].is_string() ||
         !body.contains("client") || !body["client"].is_string())) {
        error = "Informe o tipo de consulta e o paciente.";
        return false;
    }
    if (body.contains("notes") && !body["notes"].is_string()) {
        error = "O campo notes deve ser um texto.";
        return false;
    }

    appointment = body;
    if (!appointment.contains("notes")) {
        appointment["notes"] = "";
    }
    if (!appointment.contains("paymentType")) {
        appointment["paymentType"] = "particular";
    }
    if (!appointment["paymentType"].is_string()) {
        error = "Selecione Convênio ou Particular.";
        return false;
    }
    if (!appointment.contains("insuranceName")) {
        appointment["insuranceName"] = "";
    }
    if (!appointment["insuranceName"].is_string()) {
        error = "O nome do convênio deve ser um texto.";
        return false;
    }
    appointment["insuranceName"] = trimText(appointment["insuranceName"].get<std::string>());
    const auto payment_type = appointment["paymentType"].get<std::string>();
    if (payment_type != "particular" && payment_type != "convenio") {
        error = "Selecione Convênio ou Particular.";
        return false;
    }
    if (payment_type == "convenio" &&
        (appointment["insuranceName"].get<std::string>().empty() ||
         appointment["insuranceName"].get<std::string>().size() > 120)) {
        error = "Informe o nome do convênio (até 120 caracteres).";
        return false;
    }
    if (payment_type == "particular") {
        appointment["insuranceName"] = "";
    }
    if (!user.isAdmin()) {
        appointment["title"] = "Consulta médica";
        appointment["client"] = user.username;
    }
    for (const auto* field : {"title", "client"}) {
        appointment[field] = trimText(appointment[field].get<std::string>());
        const auto value = appointment[field].get<std::string>();
        if (value.empty()) {
            error = std::string("O campo ") + field + " não pode ficar vazio.";
            return false;
        }
        if (value.size() > 120) {
            error = std::string("O campo ") + field + " deve ter no máximo 120 caracteres.";
            return false;
        }
    }
    if (appointment["notes"].get<std::string>().size() > 500) {
        error = "As observações devem ter no máximo 500 caracteres.";
        return false;
    }

    const auto date = appointment["date"].get<std::string>();
    const auto start = appointment["startTime"].get<std::string>();
    const auto end = appointment["endTime"].get<std::string>();
    if (!isValidDate(date)) {
        error = "Informe uma data válida no formato AAAA-MM-DD.";
        return false;
    }
    if (!isValidTime(start) || !isValidTime(end) || start >= end) {
        error = "Informe horários válidos e uma hora final posterior à inicial.";
        return false;
    }
    return true;
}

bool readAvailabilitySlotId(
    const Json& body,
    const AuthenticatedUser& user,
    sqlite3_int64& slot_id,
    std::string& error) {
    slot_id = 0;
    if (!body.contains("availabilitySlotId")) {
        if (user.isAdmin()) {
            return true;
        }
        error = "Escolha um horário disponibilizado pelo administrador.";
        return false;
    }
    if (!body["availabilitySlotId"].is_number_integer()) {
        error = "O horário selecionado é inválido.";
        return false;
    }
    slot_id = body["availabilitySlotId"].get<sqlite3_int64>();
    if (slot_id <= 0) {
        error = "O horário selecionado é inválido.";
        return false;
    }
    return true;
}

bool hasConflict(
    const std::string& date,
    const std::string& start,
    const std::string& end,
    sqlite3_int64 excluded_id,
    sqlite3_int64 owner_user_id) {
    Statement statement(
        database.get(),
        "SELECT 1 FROM appointments "
        "WHERE appointment_date = ? AND start_time < ? AND end_time > ? "
        "AND id != ? AND owner_user_id = ? LIMIT 1");
    statement.bind(1, date);
    statement.bind(2, end);
    statement.bind(3, start);
    statement.bind(4, excluded_id);
    statement.bind(5, owner_user_id);
    const int result = sqlite3_step(statement.get());
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }
    return result == SQLITE_ROW;
}

sqlite3_int64 parseId(const std::string& value) {
    try {
        std::size_t parsed = 0;
        const auto id = std::stoll(value, &parsed);
        if (parsed != value.size() || id <= 0) {
            throw std::invalid_argument("invalid id");
        }
        return id;
    } catch (const std::exception&) {
        throw std::invalid_argument("Identificador de compromisso inválido.");
    }
}

void setJson(httplib::Response& response, const Json& body, int status) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
}

void setError(httplib::Response& response, int status, const std::string& message) {
    setJson(response, {{"error", message}}, status);
}

void handleApiError(httplib::Response& response, const std::exception& error) {
    std::cerr << "Erro na API: " << error.what() << '\n';
    setError(response, 500, "Ocorreu um erro interno. Consulte o terminal do servidor.");
}

void serveFile(httplib::Response& response, const std::string& relative_path, const char* mime_type) {
    try {
        response.set_content(
            readFile(std::filesystem::path(AGENDA_SOURCE_DIR) / "frontend" / relative_path),
            mime_type);
        response.set_header("Cache-Control", "no-store");
    } catch (const std::exception& error) {
        std::cerr << "Erro ao servir arquivo: " << error.what() << '\n';
        response.status = 500;
        response.set_content("Não foi possível carregar a interface.", "text/plain; charset=utf-8");
    }
}

void registerRoutes(httplib::Server& server) {
    server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        serveFile(response, "index.html", "text/html; charset=utf-8");
    });
    server.Get("/styles.css", [](const httplib::Request&, httplib::Response& response) {
        serveFile(response, "styles.css", "text/css; charset=utf-8");
    });
    server.Get("/app.js", [](const httplib::Request&, httplib::Response& response) {
        serveFile(response, "app.js", "text/javascript; charset=utf-8");
    });

    server.Post("/api/auth/register", [](const httplib::Request& request, httplib::Response& response) {
        try {
            const auto body = Json::parse(request.body);
            if (!body.is_object() ||
                !body.contains("username") || !body["username"].is_string() ||
                !body.contains("password") || !body["password"].is_string()) {
                setError(response, 400, "Informe usuário e senha.");
                return;
            }

            const auto username = trimText(body["username"].get<std::string>());
            const auto password = body["password"].get<std::string>();
            static const std::regex username_pattern(R"(^[A-Za-z0-9_.-]{3,64}$)");
            if (!std::regex_match(username, username_pattern)) {
                setError(response, 400, "O usuário deve ter de 3 a 64 caracteres: letras, números, ponto, hífen ou sublinhado.");
                return;
            }
            if (password.size() < 12 || password.size() > 1024) {
                setError(response, 400, "A senha deve ter entre 12 e 1024 caracteres.");
                return;
            }

            const auto token = createSessionToken();
            const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() +
                session_lifetime_seconds;
            sqlite3_int64 user_id = 0;
            {
                std::lock_guard<std::mutex> lock(database_mutex);
                executeSql("BEGIN IMMEDIATE");
                try {
                    bool username_exists = false;
                    {
                        Statement exists(database.get(), "SELECT 1 FROM users WHERE username = ?");
                        exists.bind(1, username);
                        const int result = sqlite3_step(exists.get());
                        if (result == SQLITE_ROW) {
                            username_exists = true;
                        } else if (result != SQLITE_DONE) {
                            throw std::runtime_error(sqlite3_errmsg(database.get()));
                        }
                    }
                    if (username_exists) {
                        executeSql("ROLLBACK");
                        setError(response, 409, "Esse usuário já está cadastrado. Escolha outro.");
                        return;
                    }

                    {
                        Statement insert(
                            database.get(),
                            legacy_password_hash_column
                                ? "INSERT INTO users (username, password, password_hash, role) "
                                  "VALUES (?, ?, '', 'user')"
                                : "INSERT INTO users (username, password, role) "
                                  "VALUES (?, ?, 'user')");
                        insert.bind(1, username);
                        insert.bind(2, password);
                        if (sqlite3_step(insert.get()) != SQLITE_DONE) {
                            throw std::runtime_error(sqlite3_errmsg(database.get()));
                        }
                        user_id = sqlite3_last_insert_rowid(database.get());
                    }

                    {
                        Statement create_session(
                            database.get(),
                            "INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)");
                        create_session.bind(1, hashSessionToken(token));
                        create_session.bind(2, user_id);
                        create_session.bind(3, static_cast<sqlite3_int64>(expires_at));
                        if (sqlite3_step(create_session.get()) != SQLITE_DONE) {
                            throw std::runtime_error(sqlite3_errmsg(database.get()));
                        }
                    }
                    executeSql("COMMIT");
                } catch (...) {
                    executeSql("ROLLBACK");
                    throw;
                }
            }

            setJson(response, {{"username", username}, {"role", "user"}}, 201);
            response.set_header(
                "Set-Cookie",
                "agenda_session=" + token +
                    "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                    std::to_string(session_lifetime_seconds));
        } catch (const Json::parse_error&) {
            setError(response, 400, "O corpo da requisição não contém JSON válido.");
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Post("/api/auth/login", [](const httplib::Request& request, httplib::Response& response) {
        try {
            const auto body = Json::parse(request.body);
            if (!body.is_object() ||
                !body.contains("username") || !body["username"].is_string() ||
                !body.contains("password") || !body["password"].is_string()) {
                setError(response, 400, "Informe usuário e senha.");
                return;
            }

            const auto username = trimText(body["username"].get<std::string>());
            const auto password = body["password"].get<std::string>();
            if (username.empty() || username.size() > 64 || password.empty() || password.size() > 1024) {
                setError(response, 400, "Usuário ou senha inválidos.");
                return;
            }

            LoginAccount account;
            const bool account_found = findLoginAccount(username, account);
            bool password_matches = account_found && password == account.password;
            const bool used_legacy_password_hash =
                account_found && !password_matches && !account.legacy_password_hash.empty() &&
                verifyPassword(password, account.legacy_password_hash);
            password_matches = password_matches || used_legacy_password_hash;
            if (!account_found || !password_matches) {
                setError(response, 401, "Usuário ou senha incorretos.");
                return;
            }

            if (used_legacy_password_hash) {
                std::lock_guard<std::mutex> lock(database_mutex);
                Statement migrate_password(
                    database.get(),
                    "UPDATE users SET password = ?, password_hash = '' WHERE id = ?");
                migrate_password.bind(1, password);
                migrate_password.bind(2, account.user.id);
                if (sqlite3_step(migrate_password.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
            }

            const auto token = createSessionToken();
            const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() +
                session_lifetime_seconds;
            {
                std::lock_guard<std::mutex> lock(database_mutex);
                executeSql("DELETE FROM sessions WHERE expires_at <= strftime('%s', 'now')");
                Statement statement(
                    database.get(),
                    "INSERT INTO sessions (token_hash, user_id, expires_at) "
                    "SELECT ?, id, ? FROM users WHERE id = ? AND active = 1");
                statement.bind(1, hashSessionToken(token));
                statement.bind(2, static_cast<sqlite3_int64>(expires_at));
                statement.bind(3, account.user.id);
                if (sqlite3_step(statement.get()) != SQLITE_DONE || sqlite3_changes(database.get()) == 0) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
            }

            setJson(response, {
                {"username", account.user.username},
                {"role", account.user.role}
            });
            response.set_header(
                "Set-Cookie",
                "agenda_session=" + token +
                    "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                    std::to_string(session_lifetime_seconds));
        } catch (const Json::parse_error&) {
            setError(response, 400, "O corpo da requisição não contém JSON válido.");
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Get("/api/auth/session", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!authenticatedUser(request, user)) {
                setError(response, 401, "Faça login para acessar sua agenda.");
                return;
            }
            setJson(response, {{"username", user.username}, {"role", user.role}});
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Post("/api/auth/logout", [](const httplib::Request& request, httplib::Response& response) {
        try {
            const auto token = sessionTokenFromCookie(request.get_header_value("Cookie"));
            if (!token.empty()) {
                std::lock_guard<std::mutex> lock(database_mutex);
                Statement statement(database.get(), "DELETE FROM sessions WHERE token_hash = ?");
                statement.bind(1, hashSessionToken(token));
                if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
            }
            response.status = 204;
            response.set_header(
                "Set-Cookie",
                "agenda_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Get("/api/admin/users", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!requireAdmin(request, response, user)) {
                return;
            }
            std::lock_guard<std::mutex> lock(database_mutex);
            Statement statement(
                database.get(),
                "SELECT users.id, username, role, active, "
                "(SELECT COUNT(*) FROM appointments WHERE owner_user_id = users.id) "
                "FROM users ORDER BY role, username");
            Json users = Json::array();
            int result = SQLITE_ROW;
            while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
                const auto* username = sqlite3_column_text(statement.get(), 1);
                const auto* role = sqlite3_column_text(statement.get(), 2);
                users.push_back({
                    {"id", sqlite3_column_int64(statement.get(), 0)},
                    {"username", username == nullptr ? "" : reinterpret_cast<const char*>(username)},
                    {"role", role == nullptr ? "" : reinterpret_cast<const char*>(role)},
                    {"active", sqlite3_column_int(statement.get(), 3) != 0},
                    {"appointmentCount", sqlite3_column_int64(statement.get(), 4)}
                });
            }
            if (result != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }
            setJson(response, users);
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Post("/api/admin/users", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser admin;
            if (!requireAdmin(request, response, admin)) {
                return;
            }
            const auto body = Json::parse(request.body);
            if (!body.is_object() ||
                !body.contains("username") || !body["username"].is_string() ||
                !body.contains("password") || !body["password"].is_string()) {
                setError(response, 400, "Informe um usuário e uma senha.");
                return;
            }

            const auto username = trimText(body["username"].get<std::string>());
            const auto password = body["password"].get<std::string>();
            static const std::regex username_pattern(R"(^[A-Za-z0-9_.-]{3,64}$)");
            if (!std::regex_match(username, username_pattern)) {
                setError(response, 400, "O usuário deve ter 3 a 64 caracteres: letras, números, ponto, hífen ou sublinhado.");
                return;
            }
            if (password.size() < 12 || password.size() > 1024) {
                setError(response, 400, "A senha deve ter entre 12 e 1024 caracteres.");
                return;
            }

            std::lock_guard<std::mutex> lock(database_mutex);
            Statement exists(database.get(), "SELECT 1 FROM users WHERE username = ?");
            exists.bind(1, username);
            const int exists_result = sqlite3_step(exists.get());
            if (exists_result == SQLITE_ROW) {
                setError(response, 409, "Esse usuário já existe.");
                return;
            }
            if (exists_result != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }
            Statement insert(
                database.get(),
                legacy_password_hash_column
                    ? "INSERT INTO users (username, password, password_hash, role) "
                      "VALUES (?, ?, '', 'user')"
                    : "INSERT INTO users (username, password, role) VALUES (?, ?, 'user')");
            insert.bind(1, username);
            insert.bind(2, password);
            if (sqlite3_step(insert.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }
            setJson(response, {
                {"id", sqlite3_last_insert_rowid(database.get())},
                {"username", username},
                {"role", "user"},
                {"active", true},
                {"appointmentCount", 0}
            }, 201);
        } catch (const Json::parse_error&) {
            setError(response, 400, "O corpo da requisição não contém JSON válido.");
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Put(
        R"(/api/admin/users/(\d+)/status)",
        [](const httplib::Request& request, httplib::Response& response) {
            try {
                AuthenticatedUser admin;
                if (!requireAdmin(request, response, admin)) {
                    return;
                }
                const auto id = parseId(request.matches[1].str());
                const auto body = Json::parse(request.body);
                if (!body.is_object() || !body.contains("active") || !body["active"].is_boolean()) {
                    setError(response, 400, "Informe se o acesso deve ficar ativo ou desativado.");
                    return;
                }
                const bool active = body["active"].get<bool>();
                std::lock_guard<std::mutex> lock(database_mutex);
                Statement target(database.get(), "SELECT role FROM users WHERE id = ?");
                target.bind(1, id);
                const int target_result = sqlite3_step(target.get());
                if (target_result == SQLITE_DONE) {
                    setError(response, 404, "Usuário não encontrado.");
                    return;
                }
                if (target_result != SQLITE_ROW) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                const auto* role_text = sqlite3_column_text(target.get(), 0);
                const std::string role =
                    role_text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(role_text));
                if (role == "admin") {
                    setError(response, 403, "O acesso de uma conta administradora não pode ser alterado por esta tela.");
                    return;
                }

                Statement update(database.get(), "UPDATE users SET active = ? WHERE id = ?");
                update.bind(1, static_cast<sqlite3_int64>(active ? 1 : 0));
                update.bind(2, id);
                if (sqlite3_step(update.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                if (!active) {
                    Statement revoke(database.get(), "DELETE FROM sessions WHERE user_id = ?");
                    revoke.bind(1, id);
                    if (sqlite3_step(revoke.get()) != SQLITE_DONE) {
                        throw std::runtime_error(sqlite3_errmsg(database.get()));
                    }
                }
                setJson(response, {{"id", id}, {"active", active}});
            } catch (const Json::parse_error&) {
                setError(response, 400, "O corpo da requisição não contém JSON válido.");
            } catch (const std::invalid_argument& error) {
                setError(response, 400, error.what());
            } catch (const std::exception& error) {
                handleApiError(response, error);
            }
        });

    server.Get("/api/admin/availability", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!requireAdmin(request, response, user)) {
                return;
            }
            std::lock_guard<std::mutex> lock(database_mutex);
            setJson(response, listAvailability(true));
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Post("/api/admin/availability", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser admin;
            if (!requireAdmin(request, response, admin)) {
                return;
            }
            const auto body = Json::parse(request.body);
            if (!body.is_object() ||
                !body.contains("specialty") || !body["specialty"].is_string() ||
                !body.contains("professionalName") || !body["professionalName"].is_string() ||
                !body.contains("date") || !body["date"].is_string() ||
                !body.contains("startTime") || !body["startTime"].is_string() ||
                !body.contains("endTime") || !body["endTime"].is_string()) {
                setError(response, 400, "Informe a especialidade, o nome do profissional, a data e os horários.");
                return;
            }
            const auto specialty = trimText(body["specialty"].get<std::string>());
            const auto professional_name = trimText(body["professionalName"].get<std::string>());
            const auto date = body["date"].get<std::string>();
            const auto start = body["startTime"].get<std::string>();
            const auto end = body["endTime"].get<std::string>();
            if (specialty.empty() || specialty.size() > 100) {
                setError(response, 400, "A especialidade deve ter entre 1 e 100 caracteres.");
                return;
            }
            if (professional_name.empty() || professional_name.size() > 120) {
                setError(response, 400, "O nome do profissional deve ter entre 1 e 120 caracteres.");
                return;
            }
            if (!isValidDate(date) || !isValidTime(start) || !isValidTime(end) || start >= end) {
                setError(response, 400, "Informe uma data e horários válidos.");
                return;
            }

            std::lock_guard<std::mutex> lock(database_mutex);
            if (!isFutureAvailability(date, start)) {
                setError(response, 400, "Cadastre somente horários futuros.");
                return;
            }
            if (hasAvailabilityConflict(date, start, end, specialty, professional_name)) {
                setError(response, 409, "Já existe um horário ou compromisso nesse intervalo.");
                return;
            }
            Statement appointment_conflict(
                database.get(),
                "SELECT 1 FROM appointments "
                "LEFT JOIN availability_slots ON availability_slots.id = appointments.availability_slot_id "
                "WHERE appointments.appointment_date = ? AND appointments.start_time < ? "
                "AND appointments.end_time > ? AND (availability_slots.id IS NULL "
                "OR (availability_slots.specialty = ? COLLATE NOCASE "
                "AND availability_slots.professional_name = ? COLLATE NOCASE)) LIMIT 1");
            appointment_conflict.bind(1, date);
            appointment_conflict.bind(2, end);
            appointment_conflict.bind(3, start);
            appointment_conflict.bind(4, specialty);
            appointment_conflict.bind(5, professional_name);
            const int conflict_result = sqlite3_step(appointment_conflict.get());
            if (conflict_result == SQLITE_ROW) {
                setError(response, 409, "Já existe um horário ou compromisso nesse intervalo.");
                return;
            }
            if (conflict_result != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }

            const bool has_legacy_doctor_name = tableHasColumn("availability_slots", "doctor_name");
            Statement insert(
                database.get(),
                has_legacy_doctor_name
                    ? "INSERT INTO availability_slots "
                      "(specialty, professional_name, doctor_name, appointment_date, "
                      "start_time, end_time, created_by_user_id) VALUES (?, ?, ?, ?, ?, ?, ?)"
                    : "INSERT INTO availability_slots "
                      "(specialty, professional_name, appointment_date, start_time, end_time, "
                      "created_by_user_id) VALUES (?, ?, ?, ?, ?, ?)");
            insert.bind(1, specialty);
            insert.bind(2, professional_name);
            if (has_legacy_doctor_name) {
                insert.bind(3, professional_name);
                insert.bind(4, date);
                insert.bind(5, start);
                insert.bind(6, end);
                insert.bind(7, admin.id);
            } else {
                insert.bind(3, date);
                insert.bind(4, start);
                insert.bind(5, end);
                insert.bind(6, admin.id);
            }
            if (sqlite3_step(insert.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }
            setJson(response, {
                {"id", sqlite3_last_insert_rowid(database.get())},
                {"specialty", specialty},
                {"professionalName", professional_name},
                {"date", date},
                {"startTime", start},
                {"endTime", end},
                {"isBooked", false}
            }, 201);
        } catch (const Json::parse_error&) {
            setError(response, 400, "O corpo da requisição não contém JSON válido.");
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Delete(
        R"(/api/admin/availability/(\d+))",
        [](const httplib::Request& request, httplib::Response& response) {
            try {
                AuthenticatedUser admin;
                if (!requireAdmin(request, response, admin)) {
                    return;
                }
                const auto id = parseId(request.matches[1].str());
                std::lock_guard<std::mutex> lock(database_mutex);
                Statement remove(
                    database.get(),
                    "DELETE FROM availability_slots WHERE id = ? "
                    "AND NOT EXISTS (SELECT 1 FROM appointments "
                    "WHERE appointments.availability_slot_id = availability_slots.id)");
                remove.bind(1, id);
                if (sqlite3_step(remove.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                if (sqlite3_changes(database.get()) == 0) {
                    Statement exists(database.get(), "SELECT 1 FROM availability_slots WHERE id = ?");
                    exists.bind(1, id);
                    const int result = sqlite3_step(exists.get());
                    if (result == SQLITE_ROW) {
                        setError(response, 409, "Esse horário já foi reservado e não pode ser removido.");
                    } else if (result == SQLITE_DONE) {
                        setError(response, 404, "Horário não encontrado.");
                    } else {
                        throw std::runtime_error(sqlite3_errmsg(database.get()));
                    }
                    return;
                }
                response.status = 204;
            } catch (const std::invalid_argument& error) {
                setError(response, 400, error.what());
            } catch (const std::exception& error) {
                handleApiError(response, error);
            }
        });

    server.Get("/api/admin/appointments", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!requireAdmin(request, response, user)) {
                return;
            }
            std::lock_guard<std::mutex> lock(database_mutex);
            setJson(response, listAppointments(user, true));
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Get("/api/availability", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!requireAuthentication(request, response, user)) {
                return;
            }
            std::lock_guard<std::mutex> lock(database_mutex);
            setJson(response, listAvailability(false));
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Get("/api/appointments", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!requireAuthentication(request, response, user)) {
                return;
            }
            std::lock_guard<std::mutex> lock(database_mutex);
            setJson(response, listAppointments(user));
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Get("/api/notifications", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!requireAuthentication(request, response, user)) {
                return;
            }
            std::lock_guard<std::mutex> lock(database_mutex);
            setJson(response, listNotifications(user));
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Post(
        R"(/api/appointments/(\d+)/confirm)",
        [](const httplib::Request& request, httplib::Response& response) {
            try {
                AuthenticatedUser user;
                if (!requireAuthentication(request, response, user)) {
                    return;
                }
                if (user.isAdmin()) {
                    setError(response, 403, "Somente o paciente pode confirmar a própria consulta.");
                    return;
                }
                const auto id = parseId(request.matches[1].str());
                std::lock_guard<std::mutex> lock(database_mutex);
                Statement appointment(
                    database.get(),
                    "SELECT confirmed_at, "
                    "datetime(appointment_date || ' ' || start_time) > datetime('now', 'localtime') "
                    "AND datetime(appointment_date || ' ' || start_time) "
                    "<= datetime('now', 'localtime', '+24 hours') "
                    "FROM appointments WHERE id = ? AND owner_user_id = ?");
                appointment.bind(1, id);
                appointment.bind(2, user.id);
                const int result = sqlite3_step(appointment.get());
                if (result == SQLITE_DONE) {
                    setError(response, 404, "Consulta não encontrada.");
                    return;
                }
                if (result != SQLITE_ROW) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                if (sqlite3_column_type(appointment.get(), 0) != SQLITE_NULL) {
                    const auto* timestamp = sqlite3_column_text(appointment.get(), 0);
                    setJson(response, {
                        {"appointmentId", id},
                        {"status", "confirmed"},
                        {"confirmedAt", timestamp == nullptr
                             ? Json(nullptr)
                             : Json(reinterpret_cast<const char*>(timestamp))}
                    });
                    return;
                }
                if (sqlite3_column_int(appointment.get(), 1) == 0) {
                    setError(response, 409, "A confirmação ficará disponível 24 horas antes da consulta.");
                    return;
                }

                Statement confirm(
                    database.get(),
                    "UPDATE appointments SET confirmed_at = datetime('now', 'localtime'), "
                    "updated_at = CURRENT_TIMESTAMP WHERE id = ? AND owner_user_id = ? "
                    "AND confirmed_at IS NULL");
                confirm.bind(1, id);
                confirm.bind(2, user.id);
                if (sqlite3_step(confirm.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                Statement confirmed_at(database.get(), "SELECT confirmed_at FROM appointments WHERE id = ?");
                confirmed_at.bind(1, id);
                if (sqlite3_step(confirmed_at.get()) != SQLITE_ROW) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                const auto* timestamp = sqlite3_column_text(confirmed_at.get(), 0);
                setJson(response, {
                    {"appointmentId", id},
                    {"status", "confirmed"},
                    {"confirmedAt", timestamp == nullptr
                         ? Json(nullptr)
                         : Json(reinterpret_cast<const char*>(timestamp))}
                });
            } catch (const std::invalid_argument& error) {
                setError(response, 400, error.what());
            } catch (const std::exception& error) {
                handleApiError(response, error);
            }
        });

    server.Post("/api/appointments", [](const httplib::Request& request, httplib::Response& response) {
        try {
            AuthenticatedUser user;
            if (!requireAuthentication(request, response, user)) {
                return;
            }
            const auto body = Json::parse(request.body);
            Json appointment;
            std::string validation_error;
            if (!readAppointment(body, user, appointment, validation_error)) {
                setError(response, 400, validation_error);
                return;
            }
            sqlite3_int64 slot_id = 0;
            if (!readAvailabilitySlotId(body, user, slot_id, validation_error)) {
                setError(response, 400, validation_error);
                return;
            }
            std::lock_guard<std::mutex> lock(database_mutex);
            if (slot_id > 0 && !slotIsAvailable(
                    slot_id,
                    appointment["date"].get<std::string>(),
                    appointment["startTime"].get<std::string>(),
                    appointment["endTime"].get<std::string>())) {
                setError(response, 409, "Esse horário não está mais disponível. Atualize a agenda e escolha outro.");
                return;
            }
            if (hasConflict(
                    appointment["date"].get<std::string>(),
                    appointment["startTime"].get<std::string>(),
                    appointment["endTime"].get<std::string>(),
                    0,
                    user.id)) {
                setError(response, 409, "Já existe um compromisso nesse intervalo.");
                return;
            }
            Statement statement(
                database.get(),
                "INSERT INTO appointments "
                "(title, client, appointment_date, start_time, end_time, notes, owner_user_id, "
                "availability_slot_id, payment_type, insurance_name) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
            statement.bind(1, appointment["title"].get<std::string>());
            statement.bind(2, appointment["client"].get<std::string>());
            statement.bind(3, appointment["date"].get<std::string>());
            statement.bind(4, appointment["startTime"].get<std::string>());
            statement.bind(5, appointment["endTime"].get<std::string>());
            statement.bind(6, appointment["notes"].get<std::string>());
            statement.bind(7, user.id);
            if (slot_id > 0) {
                statement.bind(8, slot_id);
            } else {
                statement.bindNull(8);
            }
            statement.bind(9, appointment["paymentType"].get<std::string>());
            statement.bind(10, appointment["insuranceName"].get<std::string>());
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }
            appointment["id"] = sqlite3_last_insert_rowid(database.get());
            appointment["ownerUsername"] = user.username;
            appointment["confirmedAt"] = nullptr;
            setJson(response, appointment, 201);
        } catch (const Json::parse_error&) {
            setError(response, 400, "O corpo da requisição não contém JSON válido.");
        } catch (const std::exception& error) {
            handleApiError(response, error);
        }
    });

    server.Put(
        R"(/api/appointments/(\d+))",
        [](const httplib::Request& request, httplib::Response& response) {
            try {
                AuthenticatedUser user;
                if (!requireAuthentication(request, response, user)) {
                    return;
                }
                const auto id = parseId(request.matches[1].str());
                const auto body = Json::parse(request.body);
                Json appointment;
                std::string validation_error;
                if (!readAppointment(body, user, appointment, validation_error)) {
                    setError(response, 400, validation_error);
                    return;
                }
                sqlite3_int64 slot_id = 0;
                if (!readAvailabilitySlotId(body, user, slot_id, validation_error)) {
                    setError(response, 400, validation_error);
                    return;
                }
                std::lock_guard<std::mutex> lock(database_mutex);
                Statement existing(
                    database.get(),
                    "SELECT owner_user_id, users.username, availability_slot_id FROM appointments "
                    "LEFT JOIN users ON users.id = appointments.owner_user_id "
                    "WHERE appointments.id = ?");
                existing.bind(1, id);
                const int existing_result = sqlite3_step(existing.get());
                if (existing_result == SQLITE_DONE) {
                    setError(response, 404, "Compromisso não encontrado.");
                    return;
                }
                if (existing_result != SQLITE_ROW) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                if (sqlite3_column_type(existing.get(), 0) == SQLITE_NULL) {
                    setError(response, 409, "O compromisso não está associado a uma conta ativa.");
                    return;
                }
                const auto owner_id = sqlite3_column_int64(existing.get(), 0);
                if (!user.isAdmin() && owner_id != user.id) {
                    setError(response, 404, "Compromisso não encontrado.");
                    return;
                }
                const auto* owner_name = sqlite3_column_text(existing.get(), 1);
                const std::string owner_username =
                    owner_name == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(owner_name));
                if (slot_id > 0 && !slotIsAvailable(
                        slot_id,
                        appointment["date"].get<std::string>(),
                        appointment["startTime"].get<std::string>(),
                        appointment["endTime"].get<std::string>(),
                        id)) {
                    setError(response, 409, "Esse horário não está mais disponível. Atualize a agenda e escolha outro.");
                    return;
                }
                if (hasConflict(
                        appointment["date"].get<std::string>(),
                        appointment["startTime"].get<std::string>(),
                        appointment["endTime"].get<std::string>(),
                        id,
                        owner_id)) {
                    setError(response, 409, "Já existe um compromisso nesse intervalo.");
                    return;
                }
                Statement statement(
                    database.get(),
                    "UPDATE appointments SET title = ?, client = ?, appointment_date = ?, "
                    "start_time = ?, end_time = ?, notes = ?, availability_slot_id = ?, "
                    "payment_type = ?, insurance_name = ?, confirmed_at = NULL, "
                    "updated_at = CURRENT_TIMESTAMP WHERE id = ?");
                statement.bind(1, appointment["title"].get<std::string>());
                statement.bind(2, appointment["client"].get<std::string>());
                statement.bind(3, appointment["date"].get<std::string>());
                statement.bind(4, appointment["startTime"].get<std::string>());
                statement.bind(5, appointment["endTime"].get<std::string>());
                statement.bind(6, appointment["notes"].get<std::string>());
                if (slot_id > 0) {
                    statement.bind(7, slot_id);
                } else {
                    statement.bindNull(7);
                }
                statement.bind(8, appointment["paymentType"].get<std::string>());
                statement.bind(9, appointment["insuranceName"].get<std::string>());
                statement.bind(10, id);
                if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                if (sqlite3_changes(database.get()) == 0) {
                    setError(response, 404, "Compromisso não encontrado.");
                    return;
                }
                appointment["id"] = id;
                appointment["ownerUsername"] = owner_username;
                appointment["availabilitySlotId"] = slot_id > 0 ? Json(slot_id) : Json(nullptr);
                appointment["confirmedAt"] = nullptr;
                setJson(response, appointment);
            } catch (const Json::parse_error&) {
                setError(response, 400, "O corpo da requisição não contém JSON válido.");
            } catch (const std::invalid_argument& error) {
                setError(response, 400, error.what());
            } catch (const std::exception& error) {
                handleApiError(response, error);
            }
        });

    server.Delete(
        R"(/api/appointments/(\d+))",
        [](const httplib::Request& request, httplib::Response& response) {
            try {
                AuthenticatedUser user;
                if (!requireAuthentication(request, response, user)) {
                    return;
                }
                const auto id = parseId(request.matches[1].str());
                std::lock_guard<std::mutex> lock(database_mutex);
                Statement statement(
                    database.get(),
                    user.isAdmin()
                        ? "DELETE FROM appointments WHERE id = ?"
                        : "DELETE FROM appointments WHERE id = ? AND owner_user_id = ?");
                statement.bind(1, id);
                if (!user.isAdmin()) {
                    statement.bind(2, user.id);
                }
                if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                if (sqlite3_changes(database.get()) == 0) {
                    setError(response, 404, "Compromisso não encontrado.");
                    return;
                }
                response.status = 204;
            } catch (const std::invalid_argument& error) {
                setError(response, 400, error.what());
            } catch (const std::exception& error) {
                handleApiError(response, error);
            }
        });
}

}  // namespace

int main() {
    try {
        initializeDatabase();
        int server_port = 8080;
        if (const char* configured_port = std::getenv("AGENDA_PORT")) {
            std::size_t parsed = 0;
            const auto port = std::stoul(configured_port, &parsed);
            if (parsed != std::string(configured_port).size() || port == 0 || port > 65535) {
                throw std::runtime_error("AGENDA_PORT deve ser um número entre 1 e 65535.");
            }
            server_port = static_cast<int>(port);
        }

        httplib::Server server;
        server.set_payload_max_length(1024 * 1024);
        registerRoutes(server);
        std::cout << "AgendaAqui disponível em http://localhost:" << server_port << '\n';
        if (!server.listen("127.0.0.1", server_port)) {
            std::cerr << "Não foi possível iniciar o servidor na porta " << server_port << ".\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "Falha ao iniciar o AgendaAqui: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
