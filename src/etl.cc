#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <iomanip>
#include <cassert>
#include <filesystem>
#include "compressor.h"
#include <cerrno>
#include <cstring>
#include <arrow/api.h>
#include <arrow/status.h>
#include <arrow/memory_pool.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

namespace fs = std::filesystem;

const int DICT_NUM_THRESHOLD = 100;
const int DICT_SAMPLE_CHUNKS = 5;
const double DICT_CHUNK_RATIO_THRESHOLD = 0.6;
const double DICT_GROUP_RATIO_THRESHOLD = 0.6;
const int ROW_GROUP_SIZE = 100000;
const int ROW_GROUPS_PER_FILE = 10;
const int COMPACTION_WINDOW = 1000000;
const int OUTLIER_THRESHOLD = 1000;
typedef std::pair<int, int> variable_t;

std::pair<int, int> variable_str_to_tup(const std::string &variable) {
    std::istringstream iss(variable);
    int a, b;
    char underscore, V;
    iss >> V >> a >> underscore >> V >> b;
    return {a, b};
}

std::map<int, std::vector<std::string>> sample_variable(size_t total_chunks, size_t group_number, const std::pair<int, int> &variable, int chunks = 2) {
    
    std::vector<std::string> paths = {};
    for (int i = 0; i < total_chunks; ++i) {
        std::string path = "compressed/" + std::to_string(group_number) + "/variable_" + std::to_string(i) + "/E" + std::to_string(variable.first) + "_V" + std::to_string(variable.second);
        if (fs::exists(path)) {
            paths.push_back(path);
            if (paths.size() == chunks) {
                break;
            }
        }
    }
    std::map<int, std::vector<std::string>> lines;
    int counter = 0;

    for (const auto &path : paths) {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            lines[counter].push_back(line);
        }
        counter++;
        file.close();
    }
    return lines;
}

std::string join(const std::vector<std::string>& vec, const std::string& delim) {
    assert(! vec.empty());
    std::string result;
    for (const auto &str : vec) {
        result += str + delim;
    }
    result.erase(result.size() - delim.size());
    return result;
}

std::pair<std::map<variable_t, int>, std::map<int, std::set<variable_t>>> get_variable_info(int total_chunks, size_t group_number) {
    std::map<variable_t, int> variable_to_type;
    std::map<int, std::set<variable_t>> chunk_variables;
    for (int chunk = 0; chunk < total_chunks; ++chunk) {
        std::string variable_tag_file = "compressed/" + std::to_string(group_number) + "/variable_" + std::to_string(chunk) + "_tag.txt";
        std::ifstream file(variable_tag_file);
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string variable_str;
            int tag;
            iss >> variable_str >> tag;
            auto variable = variable_str_to_tup(variable_str);
            variable_to_type[variable] = tag;
            chunk_variables[chunk].insert(variable);
        }
        file.close();
    }
    return {variable_to_type, chunk_variables};
}

arrow::Status write_parquet_file(size_t group_number, std::string parquet_files_prefix, size_t& parquet_file_counter, std::shared_ptr<arrow::Table> & table) {

    auto writer_properties = parquet::WriterProperties::Builder().compression(parquet::Compression::ZSTD)->build();
    std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("timestamp", arrow::utf8()),
        arrow::field("log", arrow::utf8())
    });
    std::shared_ptr<arrow::Array> timestamp_array, log_array;
    arrow::StringBuilder timestamp_builder, log_builder;
    std::ifstream timestamp_file("compressed/" + std::to_string(group_number) + "/timestamp");
    std::ifstream log_file("compressed/" + std::to_string(group_number) +"/log");
    std::string timestamp_line, log_line;

    while (std::getline(timestamp_file, timestamp_line) && std::getline(log_file, log_line)) {
        ARROW_RETURN_NOT_OK(timestamp_builder.Append(timestamp_line));
        ARROW_RETURN_NOT_OK(log_builder.Append(log_line));
    }

    ARROW_RETURN_NOT_OK(timestamp_builder.Finish(&timestamp_array));
    ARROW_RETURN_NOT_OK(log_builder.Finish(&log_array));
    std::shared_ptr<arrow::Table> this_table = arrow::Table::Make(schema, {timestamp_array, log_array});

    if (table == nullptr) {
        table = this_table;
    } else {
        ARROW_ASSIGN_OR_RAISE(table, arrow::ConcatenateTables({table, this_table}));
    }

    while (table->num_rows() >= ROW_GROUP_SIZE * ROW_GROUPS_PER_FILE) {
        // Write the table to a parquet file
        size_t write_lines = ROW_GROUP_SIZE * ROW_GROUPS_PER_FILE;
        std::string parquet_filename = parquet_files_prefix + std::to_string(parquet_file_counter++) + ".parquet";
        std::shared_ptr<arrow::io::FileOutputStream> outfile;
        ARROW_ASSIGN_OR_RAISE(outfile, arrow::io::FileOutputStream::Open(parquet_filename));
        ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(*(table->Slice(0, write_lines)), arrow::default_memory_pool(), outfile, ROW_GROUP_SIZE, writer_properties));
        table = table->Slice(write_lines);
    }

    arrow::MemoryPool* pool = arrow::default_memory_pool();
    std::cout << "Current Arrow memory usage" << pool->bytes_allocated() << std::endl;

    return arrow::Status::OK();
}

arrow::Status RunMain(int argc, char *argv[]) {

    std::string index_name = argv[1];

    std::string parquet_files_prefix = "parquets/" + index_name;

    if (fs::exists("parquets")) {
        fs::remove_all("parquets" );
    }
    fs::create_directory("parquets");

    
    std::shared_ptr<arrow::Table> table = nullptr;
    
    size_t parquet_file_counter = 0;
    
    size_t num_groups = std::stoi(argv[2]);

    const bool DEBUG = false;
    std::map<int, std::ofstream*> compacted_type_files;
    std::map<int, std::ofstream*> compacted_lineno_files;
    std::ofstream outlier_file("compressed/outlier");
    std::ofstream outlier_lineno_file("compressed/outlier_lineno");

    FILE *fp = fopen((index_name + ".maui").c_str(), "wb");
    std::vector<size_t> byte_offsets = {0};
    std::string variable_buffer = "";

    size_t current_line_number = 0;

    /* The iteration order is 1. groups 2. chunks 3. lines
    We will produce compacted type files and outlier files into the top level compressed/ directory
    We will also produce parquet files in a separate folder */

    std::map<size_t, size_t> group_chunks = {};

    for(size_t group_number = 0; group_number < num_groups; ++group_number) {

        // figure out how many chunks there are in this group
        size_t total_chunks = 0;
        while (true) {
            std::ostringstream oss;
            oss << "compressed/" + std::to_string(group_number) + "/chunk" << std::setw(4) << std::setfill('0') << total_chunks << ".eid";
            std::string chunk_filename = oss.str();
            if (std::filesystem::exists(chunk_filename)) {
                total_chunks++;
            } else {
                break;
            }
        }
        group_chunks[group_number] = total_chunks;
    }

    std::map<std::string, int> dictionary_item_occurrences = {};

    for (size_t group_number = 0; group_number < num_groups; ++group_number) {

        std::cout << "Processing group " << group_number << std::endl;

        size_t total_chunks = group_chunks[group_number];

        auto [variable_to_type, chunk_variables] = get_variable_info(total_chunks, group_number);
        std::set<variable_t> variables;
        for (const auto& kv : variable_to_type) {
            variables.insert(kv.first);
        }

        for (const auto &variable : variables) {
            auto lines = sample_variable(total_chunks, group_number, variable, DICT_SAMPLE_CHUNKS);
            std::vector<std::unordered_map<std::string, size_t>> counters = {};
            std::set<std::string> items;

            for (const auto& [key, vec] : lines) {
                std::unordered_map<std::string, size_t> counter;
                for (const auto& item : vec) {
                    counter[item]++;
                }
                counters.push_back(counter);
                items.insert(vec.begin(), vec.end());
            }

            for (const auto& item : items) {
                int num_chunks = 0;
                int num_times = 0;

                for (auto & counter: counters) {
                    if (counter[item] > 0) {
                        num_chunks++;
                        num_times += counter[item];
                    }
                }

                // if (num_times > DICT_NUM_THRESHOLD && static_cast<double>(num_chunks) / DICT_SAMPLE_CHUNKS > DICT_CHUNK_RATIO_THRESHOLD) {
                if (static_cast<double>(num_chunks) / DICT_SAMPLE_CHUNKS > DICT_CHUNK_RATIO_THRESHOLD) {
                    dictionary_item_occurrences[item]++;
                }
            }
        }
  
    }

    std::set<std::string> dictionary_items;
    // add the keys in dictionary_item_occurrences whose value exceed 
    for (const auto& [key, val] : dictionary_item_occurrences) {
        if (static_cast<double>(val) / num_groups > DICT_GROUP_RATIO_THRESHOLD) {
            dictionary_items.insert(key);
        }
    }

    for (const auto& item : dictionary_items) { std::cout << item << " ";}
    std::cout << std::endl;
    std::cout << "DICTIONARY SIZE: " << dictionary_items.size() << std::endl;

    for (size_t group_number = 0; group_number < num_groups; ++group_number) {

        size_t total_chunks = group_chunks[group_number];
        auto [variable_to_type, chunk_variables] = get_variable_info(total_chunks, group_number);

        for (const auto& [key, val] : variable_to_type) {
            std::cout << "(" << key.first << ", " << key.second << "): " << val << "; ";
        }
        std::cout << std::endl;

        std::set<variable_t> variables;
        for (const auto& kv : variable_to_type) {
            variables.insert(kv.first);
        }

        std::map<int, std::vector<variable_t>> eid_to_variables;
        std::set<int> touched_types = {0};

        for (const auto &variable : variables) {
            int eid = variable.first;
            eid_to_variables[eid].push_back(variable);
            touched_types.insert(variable_to_type[variable]);
        }

        for (const auto& [eid, vars] : eid_to_variables) {
            std::cout << "EID: " << eid << " Variables: ";
            for (const auto& var : vars) {
                std::cout << "(" << var.first << ", " << var.second << ") ";
            }
            std::cout << std::endl;
        }

        
        std::map<variable_t, std::ifstream*> variable_files = {};

        std::map<int, std::vector<std::string>> expanded_items;
        std::map<int, std::vector<size_t>> expanded_lineno;

        std::map<int, std::ofstream*> type_files;
        std::map<int, std::ofstream*> type_lineno_files;
        if (DEBUG) {
            for (const int &t : touched_types) {
                type_files[t] = new std::ofstream("compressed/" + std::to_string(group_number) + "/type_" + std::to_string(t));
                type_lineno_files[t] = new std::ofstream("compressed/" + std::to_string(group_number) + "/type_" + std::to_string(t) + "_lineno");
            }
        }

        std::string all_outliers = "";
        std::vector<size_t> outlier_linenos = {};

        arrow::Status s = write_parquet_file(group_number, parquet_files_prefix, parquet_file_counter, table);
        if (!s.ok()) { return s;}

        for (int chunk = 0; chunk < total_chunks; ++chunk) {

            for (const auto & variable: chunk_variables[chunk]) {
                std::string filename = "compressed/" + std::to_string(group_number) + "/variable_" + std::to_string(chunk) + "/E" 
                                        + std::to_string(variable.first) + "_V" + std::to_string(variable.second);
                variable_files[variable] = new std::ifstream(filename);
            }

            std::ostringstream oss;
            oss << "compressed/" + std::to_string(group_number) + "/chunk" << std::setw(4) << std::setfill('0') << chunk << ".eid";
            std::string chunk_filename = oss.str();
            std::cout << "processing chunk file: " << chunk_filename << std::endl;

            std::cout << std::filesystem::current_path() << std::endl;

            std::ifstream eid_file(chunk_filename);
            if (!eid_file.is_open()) {
                std::cerr << "Error: " << std::strerror(errno);
                std::cerr << "Failed to open file: " << chunk_filename << std::endl;
                return arrow::Status::IOError("Failed to open file: " + chunk_filename);
            }

            std::string line;
            std::vector<int> chunk_eids;
            while (std::getline(eid_file, line)) {
                chunk_eids.push_back(std::stoi(line));
            }
            eid_file.close();

            // write the parquet files first

            for (int eid : chunk_eids) {

                if ( (current_line_number + 1) % ROW_GROUP_SIZE == 0) {
                    std::cout << "current_line_number: " << current_line_number << std::endl;
                    // compress and write the variable_buffer to disk
                    Compressor compressor(CompressionAlgorithm::ZSTD);
                    std::string compressed_str = compressor.compress(variable_buffer.c_str(), variable_buffer.size());
                    fwrite(compressed_str.c_str(), sizeof(char), compressed_str.size(), fp);
                    byte_offsets.push_back(ftell(fp));
                    variable_buffer = "";
                }

                if (eid < 0) {
                    // this is an outlier. outliers will be treated separately.
                    current_line_number++;
                    continue;
                } 
                else if (eid_to_variables.find(eid) == eid_to_variables.end()) {

                    // this template does not have variables, skip it
                    current_line_number++;
                    continue;
                } 
                else {
                    auto this_variables = eid_to_variables[eid];
                    std::map<int, std::vector<std::string>> type_vars;

                    for (const auto &variable : this_variables) {
                        std::string item;
                        std::getline(*variable_files[variable], item);
                        variable_buffer += item + " ";

                        int t = (dictionary_items.find(item) != dictionary_items.end()) ? 0 : variable_to_type[variable];

                        type_vars[t].push_back(item);
                    }
                    variable_buffer += "\n";

                    for (const auto &entry : type_vars) {
                        int t = entry.first;
                        if (expanded_items.find(t) == expanded_items.end()) {
                            expanded_items[t] = {};
                            expanded_lineno[t] = {};
                        }
                        expanded_items[t].insert(expanded_items[t].end(), entry.second.begin(), entry.second.end());
                        expanded_lineno[t].resize(expanded_lineno[t].size() + entry.second.size(), current_line_number / ROW_GROUP_SIZE);
                    }

                    if (DEBUG) {
                        for (const auto &entry : type_vars) {
                            int t = entry.first;
                            
                            (*type_files[t]) << join(entry.second, " ") << "\n";
                            (*type_lineno_files[t]) << current_line_number << "\n";
                        }
                    }

                    current_line_number++;
                }
            }

            for (const int &t : touched_types) {
                if ((expanded_items[t].size() > COMPACTION_WINDOW || chunk == total_chunks - 1) && !expanded_items[t].empty()) {
                    // Sort expanded_items and expanded_lineno based on expanded_items
                    std::vector<std::pair<std::string, size_t>> paired;
                    for (size_t i = 0; i < expanded_items[t].size(); ++i) {
                        paired.emplace_back(expanded_items[t][i], expanded_lineno[t][i]);
                    }

                    std::sort(paired.begin(), paired.end(), 
                            [](const std::pair<std::string, size_t> &a, const std::pair<std::string, size_t> &b) {
                            return a.first == b.first ? a.second < b.second : a.first < b.first;
                            });

                    for (size_t i = 0; i < paired.size(); ++i) {
                        expanded_items[t][i] = paired[i].first;
                        expanded_lineno[t][i] = paired[i].second;
                    }

                    std::vector<std::string> compacted_items;
                    std::vector<std::vector<size_t>> compacted_lineno;
                    std::string last_item;

                    for (size_t i = 0; i < expanded_items[t].size(); ++i) {
                        if (expanded_items[t][i] != last_item) {
                            compacted_items.push_back(expanded_items[t][i]);
                            compacted_lineno.push_back({expanded_lineno[t][i]});
                            last_item = expanded_items[t][i];
                        } else {
                            if(expanded_lineno[t][i] != compacted_lineno.back().back()) {
                                compacted_lineno.back().push_back(expanded_lineno[t][i]);
                            }
                            // compacted_lineno.back().push_back(expanded_lineno[t][i]);
                        }
                    }

                    // Sort compacted_items and compacted_lineno based on the first element of compacted_lineno
                    // std::vector<std::pair<std::string, std::vector<size_t>>> compacted_paired;
                    // for (size_t i = 0; i < compacted_items.size(); ++i) {
                    //     compacted_paired.emplace_back(compacted_items[i], compacted_lineno[i]);
                    // }

                    // std::sort(compacted_paired.begin(), compacted_paired.end(), 
                    //         [](const std::pair<std::string, std::vector<size_t>> &a, const std::pair<std::string, std::vector<size_t>> &b) {
                    //             return a.second[0] == b.second[0] ? a.first < b.first : a.second[0] < b.second[0];
                    //         });

                    // for (size_t i = 0; i < compacted_paired.size(); ++i) {
                    //     compacted_items[i] = compacted_paired[i].first;
                    //     compacted_lineno[i] = compacted_paired[i].second;
                    // }

                    if (compacted_items.size() > OUTLIER_THRESHOLD) {
                        if (compacted_type_files[t] == nullptr) {
                            compacted_type_files[t] = new std::ofstream("compressed/compacted_type_" + std::to_string(t));
                            compacted_lineno_files[t] = new std::ofstream("compressed/compacted_type_" + std::to_string(t) + "_lineno");
                        }
                        for (size_t i = 0; i < compacted_items.size(); ++i) {
                            *compacted_type_files[t] << compacted_items[i] << "\n";

                            if (t != 0){
                                for (int num : compacted_lineno[i]) {
                                    *compacted_lineno_files[t] << num << " ";
                                }
                                *compacted_lineno_files[t] << "\n";
                            }
                        }
                    } else {
                        for (size_t i = 0; i < compacted_items.size(); ++i) {
                            outlier_file << compacted_items[i] << "\n";
                            for (int num : compacted_lineno[i]) {
                                outlier_lineno_file << num << " ";
                            }
                            outlier_lineno_file << "\n";
                        }
                    }

                    expanded_items[t].clear();
                    expanded_lineno[t].clear();
                }
            }


            for (const auto &entry : variable_files) {
                entry.second->close();
            }
        }

        if (DEBUG) {
            for (const int &t : touched_types) {
                type_files[t]->close();
                type_lineno_files[t]->close();
            }
        }
    }

    for (int t =0; t < 64; t ++) {
        if (compacted_type_files[t] != nullptr) {
            compacted_type_files[t]->close();
            delete compacted_type_files[t];
            compacted_lineno_files[t]->close();
            delete compacted_lineno_files[t];
        }
    }

    outlier_file.close();
    outlier_lineno_file.close();

    if ( variable_buffer.size() > 0 ) {
        std::cout << "current_line_number: " << current_line_number << std::endl;
        // compress and write the variable_buffer to disk
        Compressor compressor(CompressionAlgorithm::ZSTD);
        std::string compressed_str = compressor.compress(variable_buffer.c_str(), variable_buffer.size());
        fwrite(compressed_str.c_str(), sizeof(char), compressed_str.size(), fp);
        variable_buffer = "";
    }
    fclose(fp);

    auto writer_properties = parquet::WriterProperties::Builder().compression(parquet::Compression::ZSTD)->build();
    std::string parquet_filename = parquet_files_prefix + std::to_string(parquet_file_counter++) + ".parquet";
    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    ARROW_ASSIGN_OR_RAISE(outfile, arrow::io::FileOutputStream::Open(parquet_filename));
    ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, ROW_GROUP_SIZE, writer_properties));

    return arrow::Status::OK();
}

int main(int argc, char *argv[]) {
    
    arrow::Status s = RunMain(argc, argv);
    if (!s.ok()) {
        std::cout << s.ToString() << "\n";
    }
    return 0;
}