#include "config/config_file_parser.hpp"
#include "assertUtils.hpp"

#include <iostream>
#include <string>

using std::cout;
using std::cin;
using std::string;
using std::vector;

int main(int argc, char **argv) {
  cout << "Enter configuration file name with a full/relative path,"
       << " or 'd' for a default:\n";

  const string default_config_file = "scripts/sample_config.txt";
  string config_file = default_config_file;
  string given_config_file;
  const string use_default_config_file = "d";
  const uint expected_replicas_num = 3; //4;
  const uint expected_clients_num = 2;
  const string expected_replica1 = "10.197.133.21:3410"; // "127.0.0.1:3410";
  const string expected_replica2 = "10.197.133.21:3420"; // "127.0.0.1:3420";
  const string expected_replica3 = "10.197.133.21:3430"; // "127.0.0.1:3430";
  const string expected_replica4 = "10.197.133.21:3440"; // "127.0.0.1:3440";
  const string expected_client = "34.122.181.149:4444"; // "127.0.0.1:4444";
  const string values_to_split = "10.23.43.1:1234:1238";
  const string expected_split_values[] = {"10.23.43.1", "1234", "1238"};
  const string values_to_split_delimiter = ":";

  cin >> given_config_file;
  if (given_config_file != use_default_config_file) config_file = given_config_file;
  logging::Logger logger = logging::getLogger("simpletest.test");
  ConfigFileParser parser(logger, config_file);
  if (!parser.Parse()) return 1;

  cout << "\n";
  size_t replicas_num = parser.Count("replicas_config");
  vector<string> replicas = parser.GetValues("replicas_config");

  size_t clients_num = parser.Count("clients_config");
  vector<string> clients = parser.GetValues("clients_config");
  parser.printAll();

  vector<std::string> split_values_vector = parser.SplitValue(values_to_split, values_to_split_delimiter.c_str());

  if (config_file == default_config_file) {
    Assert(replicas_num == expected_replicas_num);
    Assert(clients_num == expected_clients_num);
    Assert(expected_replica1 == replicas[0]);
    Assert(expected_replica2 == replicas[1]);
    Assert(expected_replica3 == replicas[2]);
    Assert(expected_replica4 == replicas[3]);
    Assert(expected_client == clients[0]);
    Assert(split_values_vector.size() == sizeof(expected_split_values) / sizeof(expected_split_values[0]));
    Assert(split_values_vector[0] == expected_split_values[0]);
    Assert(split_values_vector[1] == expected_split_values[1]);
    Assert(split_values_vector[2] == expected_split_values[2]);
  }
  return 0;
}
