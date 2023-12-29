#include <iostream>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

#include "filesystem.hpp"

namespace fs = ghc::filesystem;

struct Buffer {
	std::vector<unsigned char> data;
	uint32_t cursor{0};

	template<typename T>
	T read(){
		T val = *(reinterpret_cast<T*>(&data[cursor]));
		cursor += sizeof(T) / sizeof(unsigned char);
		return val;
	}

	template<typename T>
	void write(T val){
		*(reinterpret_cast<T*>(&data[cursor])) = val;
		cursor += sizeof(T) / sizeof(unsigned char);
	}

	uint32_t readUint24_t(){
		uint32_t value = read<uint16_t>();
		value |= read<unsigned char>() << 16;
		return value;
	}

	void writeUint24_t(uint32_t val){
		uint16_t v0 = val & 0xFFFF;
		unsigned char v1 = (val >> 16) & 0xFF;
		write<uint16_t>(v0);
		write<unsigned char>(v1);
	}

	template<typename T>
	bool contains(){
		return cursor + sizeof(T) / sizeof(unsigned char) < data.size();
	}

	std::string readString(uint32_t size){
		std::string str;
		str.resize(size);
		for(uint32_t i = 0; i < size; ++i){
			str[i] = read<char>();
		}
		return str;
	}

	void writeString(const std::string& str, uint32_t maxSize){
		const uint32_t charCount = std::min(uint32_t(str.size()), maxSize);
		for(uint32_t i = 0; i < charCount; ++i){
			write<char>(str[i]);
		}
	}

	void resize(uint32_t sizeInBytes){
		data.resize(sizeInBytes);
	}

};

enum ScriptType {
	kScriptTypeNode,
	kScriptTypeAmbientSound,
	kScriptTypeBackgroundSound,
	kScriptTypeNodeInit,
	kScriptTypeAmbientCue
};

struct ScriptLocation {
	ScriptType type;
	uint32_t offset;
	uint32_t size;
};

using ScriptIndex = std::vector<ScriptLocation>;

void readScriptIndex(Buffer& buffer, ScriptIndex& scripts){
	uint32_t count = buffer.read<uint32_t>();
	for (uint i = 0; i < count; i++) {
		std::string roomName = buffer.readString(5);
		
		ScriptType type = (ScriptType)buffer.read<uint32_t>(); // type
		
		uint32_t offset = buffer.read<uint32_t>(); 
		uint32_t size = buffer.read<uint32_t>();

		if(type == kScriptTypeAmbientSound || type == kScriptTypeAmbientCue || type == kScriptTypeBackgroundSound){
			continue;
		}
		scripts.push_back({type, offset, size});
	}
}

void readAudioBank(Buffer& buffer){
	uint32_t count = buffer.read<uint32_t>();
	for (uint32_t i = 0; i < count; i++) {
		uint32_t id = buffer.read<uint32_t>();
		std::string soundName = buffer.readString(32);
	}
}

void parseOpcodes(Buffer& buffer, uint32_t end){
	while(buffer.cursor < end){
		uint16_t code = buffer.read<uint16_t>();
		uint8_t op = code & 0xff;
		uint8_t count = code >> 8;
		if (count == 0 && op == 0){
			break;
		}

		if(op == 16){
			std::cout << "Found opcode " << int(op) << " with values: ";
		}

		for (int i = 0; i < count; i++) {
			int16_t value = buffer.read<int16_t>();

			if((op == 16) && (i >= 2) && (i<=5)){
				std::cout << value << ", ";
				buffer.cursor -= sizeof(int16_t);
				buffer.write<int16_t>(4 * value);
			}
		}

		if(op == 16){
			std::cout << std::endl;
		}
	}
}

void parseNode(Buffer& buffer, uint32_t end){
	while(buffer.cursor < end) {
		int16_t condition = buffer.read<int16_t>();
		if(!condition){
			break;
		}
		// Workaround
		if (condition == 565) {
			buffer.cursor -= 2;
		}
		parseOpcodes(buffer, end);
	}

	while(buffer.cursor < end) {
		int16_t condition = buffer.read<int16_t>();
		if(!condition){
			break;
		}
		if (condition != -1) {
			// Rectangles
			do {
				buffer.read<uint16_t>();//rect.centerPitch =
				buffer.read<uint16_t>();//rect.centerHeading
				int16_t width = buffer.read<int16_t>();//rect.width 
				buffer.read<uint16_t>();//rect.height
				if (width >= 0){
					break;
				}
			} while (buffer.cursor < end);

			buffer.read<uint16_t>(); //cursor
		}
		parseOpcodes(buffer, end);
		
	}
}

int main(int argc, char** argv){

	if(argc < 2){
		std::cout << "executable path/to/input/file.dat" << std::endl;
		return 0;
	}

	const fs::path inFilePath(argv[1]); 
	fs::path outFilePath = inFilePath;
	outFilePath.replace_filename(inFilePath.stem().string() + "_test.dat");
	
	std::cout << "Processing scripting file: " << outFilePath.generic_string() << std::endl;

	Buffer buffer;
	{
		FILE* inFile = fopen(inFilePath.c_str(), "rb");
		if(!inFile){
			return -1;
		}
		// Start by copying everything
		fseek(inFile, 0, SEEK_END);
		size_t fileSize = ftell(inFile);
	
		buffer.resize(fileSize);

		fseek(inFile, 0L, SEEK_SET);
		fread(buffer.data.data(), sizeof(unsigned char), buffer.data.size(), inFile);
		fclose(inFile);
	}
		// Correct size and offset of opcode 16.
	{
		ScriptIndex scripts;
		buffer.read<uint32_t>(); // Magic
		buffer.read<uint32_t>(); // version
		readScriptIndex(buffer, scripts);  // Main scripts
		readScriptIndex(buffer, scripts); // Menu scripts 6 languages version
		readScriptIndex(buffer, scripts); // Menu scripts 2 languages CD version
		readScriptIndex(buffer, scripts); // Menu scripts english CD version
		readScriptIndex(buffer, scripts);  // Main scripts Xbox version
		readScriptIndex(buffer, scripts); // Menu scripts PAL Xbox version
		readScriptIndex(buffer, scripts); // Menu scripts NTSC Xbox version
		readAudioBank(buffer);   // Sound names
		readAudioBank(buffer);   // Sound names Xbox

		std::cout << "Found " << scripts.size() << " script entries." << std::endl;
	
		uint32_t scriptsOffset = buffer.cursor;

		for(const ScriptLocation& script : scripts){
			uint32_t start = scriptsOffset + script.offset;
			uint32_t end = start + script.size;
			buffer.cursor = start;

			if(script.type == kScriptTypeNodeInit){
				parseOpcodes(buffer, end);
			} else {
				while(buffer.cursor < end){
					int16_t id = buffer.read<int16_t>();

					// End of list
					if (id == 0)
						break;

					if (id > 0) {
						parseNode(buffer, end);

					} else {
						if (id == -10){
							do {
								id = buffer.read<int16_t>();
								if (id < 0) {
									buffer.read<int16_t>(); // end
								} 
							} while (id);
						} else {
							for (int i = 0; i < -id; i++) {
								buffer.read<uint16_t>();
							}
						}

						// Load the script
						parseNode(buffer, end);
						
					}
				}
			}
		}

	}

	{
		FILE* outFile = fopen(outFilePath.c_str(), "wb");
		if(!outFile){
			return -1;
		}
		fwrite(buffer.data.data(), sizeof(unsigned char), buffer.data.size(), outFile);
		fclose(outFile);
	}
	return 0;
}