#include <iostream>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

#include "libs/filesystem.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#define STBI_ONLY_JPEG

#include "libs/stb_image.h"
#include "libs/stb_image_resize.h"
#include "libs/stb_image_write.h"

namespace fs = ghc::filesystem;

#define UPSCALE_FACTOR 4

enum ResourceType {
		kCubeFace           =  0,
		kWaterEffectMask    =  1,
		kLavaEffectMask     =  2,
		kMagneticEffectMask =  3,
		kShieldEffectMask   =  4,
		kSpotItem           =  5,
		kFrame              =  6,
		kRawData            =  7,
		kMovie              =  8,
		kStillMovie         = 10,
		kText               = 11,
		kTextMetadata       = 12,
		kNumMetadata        = 13,
		kLocalizedSpotItem  = 69,
		kLocalizedFrame     = 70,
		kMultitrackMovie    = 72,
		kDialogMovie        = 74
};

std::unordered_map<ResourceType, std::string> resourceNames = {
	{ kCubeFace          , "kCubeFace" }, 
	{ kWaterEffectMask   , "kWaterEffectMask" }, 
	{ kLavaEffectMask    , "kLavaEffectMask" }, 
	{ kMagneticEffectMask, "kMagneticEffectMask" }, 
	{ kShieldEffectMask  , "kShieldEffectMask" }, 
	{ kSpotItem          , "kSpotItem" }, 
	{ kFrame             , "kFrame" }, 
	{ kRawData           , "kRawData" }, 
	{ kMovie             , "kMovie" }, 
	{ kStillMovie        , "kStillMovie" }, 
	{ kText              , "kText" }, 
	{ kTextMetadata      , "kTextMetadata" }, 
	{ kNumMetadata       , "kNumMetadata" }, 
	{ kLocalizedSpotItem , "kLocalizedSpotItem" }, 
	{ kLocalizedFrame    , "kLocalizedFrame" }, 
	{ kMultitrackMovie   , "kMultitrackMovie" }, 
	{ kDialogMovie       , "kDialogMovie" }, 
};

std::string getResourceTypeName(ResourceType type){
	if(resourceNames.count(type) == 0){
		return "Unknown";
	}
	return resourceNames.at(type);
}

struct SubEntry {
	std::vector<uint32_t> metadata;
	std::vector<unsigned char> data;
	ResourceType type;
	uint32_t offset;
	uint32_t size;
	unsigned char face;
};

struct Entry {
	std::vector<SubEntry> subEntries;
	std::string name;
	uint32_t index; // uint24_t
};

struct Directory {
	std::vector<Entry> entries;
	uint32_t size;
	bool encoded;
};

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

bool decryptHeader(FILE* file, Buffer& buffer) {
	static const uint32_t addKey = 0x3C6EF35F;
	static const uint32_t multKey = 0x0019660D;

	fseek(file, 0, SEEK_SET);

	uint32_t data;
	uint32_t size = 0;
	fread(&size, sizeof(uint32_t), 1, file);

	bool encrypted = size > 1000000;
	fseek(file, 0, SEEK_SET);

	if(encrypted) {
		uint32_t decryptedSize = size ^ addKey;
		buffer.resize(decryptedSize * sizeof(uint32_t));

		uint32_t currentKey = 0;
		for (uint32_t i = 0; i < decryptedSize; ++i) {
			currentKey += addKey;
			fread(&data, sizeof(uint32_t), 1, file);
			buffer.write<uint32_t>(data ^ currentKey);
			currentKey *= multKey;
		}
	} else {
		buffer.resize(size * sizeof(uint32_t));
		fread(buffer.data.data(), sizeof(uint32_t), size, file);
	}
	buffer.cursor = 0;
	return encrypted;
}

void encryptHeader(Buffer& buffer, FILE* file) {
	static const uint32_t addKey = 0x3C6EF35F;
	static const uint32_t multKey = 0x0019660D;

	fseek(file, 0, SEEK_SET);
	buffer.cursor = 0;

	// 34e35882		// 512a91e7
	// e7d60f6e		// 8c00d905
	uint32_t size = buffer.data.size() / sizeof(uint32_t);

	uint32_t currentKey = 0;
	for (uint32_t i = 0; i < size; ++i) {
		currentKey += addKey;
		uint32_t data = buffer.read<uint32_t>() ^ currentKey;
		fwrite(&data, sizeof(uint32_t), 1, file);
		currentKey *= multKey;
	}
}

void readSubEntry(Buffer &buffer, SubEntry& subEntry) {
	
	subEntry.offset = buffer.read<uint32_t>();

	subEntry.size = buffer.read<uint32_t>();
	uint16_t metadataSize = buffer.read<uint16_t>();

	subEntry.face = buffer.read<unsigned char>();
	subEntry.type = (ResourceType)buffer.read<unsigned char>();

	// Metadata blocks are using size and offset fields to store metadata.
	if(subEntry.type != kNumMetadata && subEntry.type != kTextMetadata){
		subEntry.data.resize(subEntry.size);
	}
	subEntry.metadata.resize(metadataSize);

	for (uint i = 0; i < metadataSize; ++i) {
		subEntry.metadata[i] = buffer.read<uint32_t>();
	}

}

void readEntry(Buffer &buffer, Entry& entry, bool expectNames) {
		
	if(expectNames){
		entry.name = buffer.readString(4);
	}
	
	entry.index = buffer.readUint24_t();

	unsigned char subItemCount = buffer.read<unsigned char>();
		
	entry.subEntries.resize(subItemCount);

	for (uint i = 0; i < subItemCount; i++) {
		readSubEntry(buffer, entry.subEntries[i]);
	}

}

void readDirectory(FILE* file, Directory& directory, bool expectNames) {
	Buffer buffer;

	directory.encoded = decryptHeader(file, buffer);
	directory.size = buffer.read<uint32_t>();
	assert(directory.size * sizeof(uint32_t) == buffer.data.size());

	while(buffer.contains<uint32_t>()) {
		directory.entries.emplace_back();
		readEntry(buffer, directory.entries.back(), expectNames);
	}
}


void writeSubEntry(const SubEntry& subEntry, Buffer &buffer) {
	
	buffer.write<uint32_t>(subEntry.offset);

	buffer.write<uint32_t>(subEntry.size);
	buffer.write<uint16_t>(subEntry.metadata.size());

	buffer.write<unsigned char>(subEntry.face);
	buffer.write<unsigned char>(subEntry.type);

	for (uint i = 0; i < subEntry.metadata.size(); i++) {
		buffer.write<uint32_t>(subEntry.metadata[i]);
	}

}

void writeEntry(const Entry& entry, Buffer &buffer) {
		
	if(!entry.name.empty()){
		buffer.writeString(entry.name, 4);
	}
	buffer.writeUint24_t(entry.index);
	buffer.write<unsigned char>(entry.subEntries.size());
	
	for (const SubEntry& subEntry : entry.subEntries) {
		writeSubEntry(subEntry, buffer);
	}

}

void writeDirectory(const Directory& directory, FILE* file) {
	Buffer buffer;
	buffer.resize(directory.size * sizeof(uint32_t));
	buffer.write<uint32_t>(directory.size);
	
	for(const Entry& entry : directory.entries){
		writeEntry(entry, buffer);
	}
	assert(buffer.data.size() == directory.size * sizeof(uint32_t));

	if(directory.encoded){
		encryptHeader(buffer, file);
	} else {
		fwrite(buffer.data.data(), sizeof(unsigned char), buffer.data.size(), file);
	}
}

void logDirectory(const Directory& directory) {
	std::cout << "Directory: size: " << directory.size << ", " << (directory.encoded ? "encoded" : "readable") << std::endl;

	for(const Entry& entry : directory.entries){
		std::cout << "* Entry: \"" << entry.name << "\", index:" << entry.index << std::endl;

		for(const SubEntry& subEntry : entry.subEntries){
			std::cout << "\t* Subentry: " << getResourceTypeName(subEntry.type) << ", face " << int(subEntry.face) << ", offset:" << subEntry.offset << ", size:" << subEntry.size << std::endl;
			
			const size_t metadataToDisplayCount = std::min(size_t(4), subEntry.metadata.size());
			if(metadataToDisplayCount != 0){
				std::cout << "\t\tMetadata (" << subEntry.metadata.size() << ")";
				for(size_t i = 0; i < metadataToDisplayCount; ++i){
					std::cout << ", " << subEntry.metadata[i];
				}
				std::cout << std::endl;
			}
		}
	} 
}

void writeJPEGToEntryFunc(void *context, void *data, int size){
	std::vector<unsigned char>& vector = *((std::vector<unsigned char>*)context);

	size_t prevSize = vector.size();
	vector.resize(prevSize + size);
	memcpy(&vector[prevSize], data, size);
 }


int main(int argc, char** argv){

	if(argc < 4){
		std::cout << "executable path/to/input_dir path/to/output_dir subpath/to/nodes.m3a [-names]" << std::endl;
		return 0;
	}

	const fs::path inputDir(argv[1]); 
	const fs::path upscaledDir(argv[2]); 
	const fs::path outputDir(argv[3]); 
	const fs::path inputFile(argv[4]); 
	const fs::path relativeFile = inputFile.lexically_relative(inputDir);

	bool expectNames = false;
	bool passthrough = false;

	for(int i = 5; i < argc; ++i){
		const std::string arg(argv[i]);
		if(arg == "-names"){
			expectNames = true;
		} else if(arg == "-passthrough"){
			passthrough = true;
		}
	}

	const fs::path inFilePath = inputDir / relativeFile;
	const fs::path outFilePath = outputDir / relativeFile;
	fs::create_directories(outFilePath.parent_path());

	// Parse input file.
	Directory directory;

	{
		FILE* inFile = fopen(inFilePath.c_str(), "rb");

		if(!inFile){
			return -1;
		}

		readDirectory(inFile, directory, expectNames);


#define LOG_ENTRIES
#ifdef LOG_ENTRIES
	logDirectory(directory);
#endif
	
		// Load corresponding data.
		for(Entry& entry : directory.entries){
			for(SubEntry& subEntry : entry.subEntries){
				if(subEntry.data.empty()){
					continue;
				}
				assert(subEntry.data.size() == subEntry.size);
				fseek(inFile, subEntry.offset, SEEK_SET);
				fread(subEntry.data.data(), sizeof(unsigned char), subEntry.data.size(), inFile);
			}
		} 

		fclose(inFile);
	}

	// Modify data in some entries (and metadata?)
	if(!passthrough){
		bool dataModified = false;
		
		fs::path parentDirectory = relativeFile.parent_path();
		std::string baseFileName = relativeFile.stem().string();
		std::string baseExtension = relativeFile.extension().string(); // including "."
		if(!baseExtension.empty() && baseExtension[0] == '.'){
			baseExtension = baseExtension.substr(1);
		}
		fs::path upscaledArchivePath = upscaledDir / parentDirectory / (baseFileName + "-" + baseExtension);

		std::cout << "Searching for upscaled data in " << upscaledArchivePath.generic_string() << std::endl;

		const std::string defaultEntryName = baseFileName.substr(0,4);
		const std::string cubeSuffixes[] = {"", "back", "bottom", "front", "left", "right", "top"};

		// * For each subentry, find the corresponding file on disk.
		for(Entry& entry : directory.entries){
			const std::string& entryName = entry.name.empty() ? defaultEntryName : entry.name;
			const std::string entryFullName = entryName + "-" + std::to_string(entry.index);

			for(SubEntry& subEntry : entry.subEntries){
				// No data to update.
				if(subEntry.data.empty()){
					continue;
				}
				// Rescale spot items
				if(subEntry.type == kSpotItem || subEntry.type == kLocalizedSpotItem){
					subEntry.metadata[0] *= UPSCALE_FACTOR;
					subEntry.metadata[1] *= UPSCALE_FACTOR;
				}
				std::string fileName;
				if(subEntry.type == kSpotItem){
					fileName = entryFullName + "-" + std::to_string(subEntry.type) + "-" + std::to_string(subEntry.face) + "-edit.jpeg";
				}
				if(subEntry.type == kLocalizedSpotItem || subEntry.type == kLocalizedFrame){
					fileName = entryFullName + "-" + std::to_string(subEntry.type - 24) + "-" + std::to_string(subEntry.face) + "-edit.jpeg";
				
				}
				if(subEntry.type == kFrame){
					fileName = entryFullName + "-" + std::to_string(subEntry.type) + "-edit.jpeg";
				}
				if(subEntry.type == kCubeFace){
					fileName = entryFullName + "-" + cubeSuffixes[subEntry.face] + "-edit.jpeg";
				}
				if(fileName.empty()){
					continue;
				}
				const fs::path upscaledFilePath = upscaledArchivePath / fileName;

				std::cout << "- Looking for file: " << fileName << "...";

				if(fs::exists(upscaledFilePath)){
					// * If exists, load it (jpeg only)
					FILE* upFile = fopen(upscaledFilePath.c_str(), "rb");
					if(upFile){
						std::cout << " OK" << std::endl;

						// * Update data size
						fseek(upFile, 0, SEEK_END);
						subEntry.data.resize(ftell(upFile));
						subEntry.size = subEntry.data.size();
						// * Copy jpeg blob.
						fseek(upFile, 0L, SEEK_SET);
						fread(subEntry.data.data(), sizeof(unsigned char), subEntry.data.size(), upFile);
						fclose(upFile);
						// Next item
						dataModified = true;
						continue;
					}
				}

				// If we reached this path, the file didn't exist, we have to upscale manually.
				std::cout << "  X Falling back to basic upscaling." << std::endl;
				
				int w, h, c;
				const int tgtChannels = 3;

				stbi_uc* decodedImg = stbi_load_from_memory(subEntry.data.data(), subEntry.data.size(), &w, &h, &c, tgtChannels);
				if(!decodedImg){
					std::cout << "Unable to decode JPEG file" << std::endl;
					continue;
				}

				unsigned int tgtWidth  = UPSCALE_FACTOR * w;
				unsigned int tgtHeight = UPSCALE_FACTOR * h;
				std::vector<unsigned char> upscaledImg(tgtWidth * tgtHeight * tgtChannels);
#define SMOOTH_RESIZE
#ifdef SMOOTH_RESIZE
				int res = stbir_resize_uint8(decodedImg, w, h, 0, upscaledImg.data(), tgtWidth, tgtHeight, 0, tgtChannels);
				if(res == 0){
					std::cout << "Unable to uscale image" << std::endl;
					continue;
				}
#else
				for(uint32_t y = 0; y < h; ++y){
					uint32_t rowSrcIndex = y * w * tgtChannels;

					for(uint32_t x = 0; x < w; ++x){
						uint32_t baseSrcIndex = rowSrcIndex + x * tgtChannels;

						unsigned char rgb[3];
						for(uint32_t i = 0; i < tgtChannels; ++i){
							rgb[i] = decodedImg[baseSrcIndex + i];
						}

						for(uint dy = 0; dy < UPSCALE_FACTOR; ++dy){
							uint32_t rowDstIndex = (y + dy) * tgtWidth * tgtChannels;
							for(uint dx = 0; dx < UPSCALE_FACTOR; ++dx){
								uint32_t baseDstIndex = rowDstIndex + (x + dx) * tgtChannels;
								for(uint32_t i = 0; i < tgtChannels; ++i){
									upscaledImg[baseDstIndex + i] = rgb[i];
								}
							}
						}
					}
				}

#endif
				stbi_image_free(decodedImg);
				std::vector<unsigned char> encodedUpscaledImg;

				res = stbi_write_jpg_to_func(writeJPEGToEntryFunc, (void*)&encodedUpscaledImg, tgtWidth, tgtHeight, tgtChannels, upscaledImg.data(), 100 /* max quality */);
				if(res == 0){
					std::cout << "Unable to encode JPEG" << std::endl;
					continue;
				}
				// Update entry.
				subEntry.data = encodedUpscaledImg;
				subEntry.size = subEntry.data.size();
					
				dataModified = true;
			}
		}

		// Update offsets
		// The first blob goes after the header, which won't change size fortunately.
		if(dataModified){
			uint32_t currentOffset = directory.size * sizeof(uint32_t);
			// Then append in the same order.
			for(Entry& entry : directory.entries){
				for(SubEntry& subEntry : entry.subEntries){
					if(subEntry.data.empty()){
						continue;
					}
					subEntry.offset = currentOffset;
					currentOffset += subEntry.data.size();
				}
			}
		}
	}
	
	// Now pack and encode the header.

	FILE* outFile = fopen(outFilePath.c_str(), "wb");
	writeDirectory(directory, outFile);
	// Write corresponding data
	for(const Entry& entry : directory.entries){
		for(const SubEntry& subEntry : entry.subEntries){
			if(subEntry.data.empty()){
				continue;
			}
			fseek(outFile, subEntry.offset, SEEK_SET);
			fwrite(subEntry.data.data(), sizeof(unsigned char), subEntry.data.size(), outFile);
		}
	}
	fclose(outFile);


	return 0;
}