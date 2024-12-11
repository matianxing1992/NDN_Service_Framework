#ifndef NDN_SERVICE_FRAMEWORK_BLOOM_FILTER_HPP
#define NDN_SERVICE_FRAMEWORK_BLOOM_FILTER_HPP

#include <iostream>
#include <vector>
#include <string>
#include <bitset>
#include <functional>
#include <sstream>
#include <iomanip>

namespace ndn_service_framework{

    class BloomFilter {
    private:
        std::vector<bool> bitArray;
        std::function<size_t(const std::string&)> hashFunction1;
        std::function<size_t(const std::string&)> hashFunction2;
        std::function<size_t(const std::string&)> hashFunction3;

    public:
        BloomFilter(size_t size = 32) : bitArray(size) {
            hashFunction1 = [](const std::string& str) { return std::hash<std::string>{}(str); };
            hashFunction2 = [](const std::string& str) { return std::hash<std::string>{}(str + "hash2"); };
            hashFunction3 = [](const std::string& str) { return std::hash<std::string>{}(str + "hash3"); };
        }

        void insert(const std::string& str) {
            size_t index1 = hashFunction1(str) % bitArray.size();
            size_t index2 = hashFunction2(str) % bitArray.size();
            size_t index3 = hashFunction3(str) % bitArray.size();
            bitArray[index1] = true;
            bitArray[index2] = true;
            bitArray[index3] = true;
        }

        bool contains(const std::string& str) {
            size_t index1 = hashFunction1(str) % bitArray.size();
            size_t index2 = hashFunction2(str) % bitArray.size();
            size_t index3 = hashFunction3(str) % bitArray.size();
            return bitArray[index1] && bitArray[index2] && bitArray[index3];
        }

        // turn the BF into a hex string   
        std::string toHexString() {
            std::stringstream hexStringStream;
            for (size_t i = 0; i < bitArray.size(); i += 8) {
                uint8_t byte = 0;
                for (size_t j = 0; j < 8 && i + j < bitArray.size(); ++j) {
                    byte |= (bitArray[i + j] << (7 - j));
                }
                hexStringStream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
            }
            return hexStringStream.str();
        }

        bool fromHexString(const std::string& hexString) {
            // Check if the hexString length is even
            if (hexString.size() % 2 != 0) {
                std::cerr << "Error: Invalid hexadecimal string length" << std::endl;
                return false;
            }

            bitArray.clear();
            for (size_t i = 0; i < hexString.size(); i += 2) {
                // Extract two characters from hexString and convert them to uint8_t
                uint8_t byte;
                try {
                    byte = std::stoi(hexString.substr(i, 2), nullptr, 16);
                } catch (const std::invalid_argument& e) {
                    std::cerr << "Error: Invalid hexadecimal string - contains non-hexadecimal characters" << std::endl;
                    return false;
                } catch (const std::out_of_range& e) {
                    std::cerr << "Error: Invalid hexadecimal string - out of range" << std::endl;
                    return false;
                }

                // Convert each bit of byte to bool and append to bitArray
                for (int j = 7; j >= 0; --j) {
                    bitArray.push_back((byte >> j) & 1);
                }
            }
            return true;
        }
                     
    };
}
#endif // NDN_SERVICE_FRAMEWORK_BLOOM_FILTER_HPP

