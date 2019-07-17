#include "AudioClip.hpp"

struct FolderReader
{
	FolderReader()
	{
		audioClips_.push_back(AudioClip());
		displayNames_.push_back("No sample");
		fileNames_.push_back("");
	}

	void scanDirectory(std::string directory)
	{
		// First two entries are:
		// .
		// ..
		DIR *dir;

		if ((dir = opendir(directory.c_str())) == NULL)
			return;

		struct dirent *ent;
		audioClips_.clear();
		fileNames_.clear();
		displayNames_.clear();

		while ((ent = readdir(dir)) != NULL)
		{
			std::string fileName = ent->d_name;
			if (fileName != ".." && fileName != ".") // Dont add invalid entries.
			{
				// Only add .wav files
				std::size_t found = fileName.find(".wav", fileName.length() - 5);
				// and .WAV files
				if (found == std::string::npos)
					found = fileName.find(".WAV", fileName.length() - 5);

				if (found != std::string::npos)
				{
					std::string path = directory + "/" + fileName;

					fileNames_.push_back(path);
					std::string basename = string::filenameBase(string::filename(path)); // no extension
					std::string displayText = short_fileName(basename);
					displayNames_.push_back(displayText);
				}
			}
		}

		maxFileIndex_ = fileNames_.size() - 1;
		loadDirectoryClips();
	}

	void getNewSavePath(std::string& path, int& number_of_files)
	{
		std::string directory = string::directory(fileNames_[0]);

		number_of_files = maxFileIndex_ + 1;

		std::string nameText = "Record" + std::to_string(number_of_files);
		std::string filename = nameText + ".wav";

		path = directory + "/" + filename;
	}

	unsigned int findFileNameIndex(std::string fileName)
	{
		for (unsigned int i = 0; i < fileNames_.size(); i++)
		{
			if (fileName == fileNames_[i])
				return i;
		}
		
		return 0;
	}

	void reScanDirectory()
	{
		std::string directory = string::directory(fileNames_[0]);
		scanDirectory(directory);
	}

	std::vector<AudioClip> audioClips_;
	std::vector<std::string> fileNames_;
	std::vector<std::string> displayNames_;
	int maxFileIndex_ = 0;

private:

	static std::string short_fileName(const std::string &filename, int maxCharacters = 13)
	{
		const int characterCount = filename.size();

		if (characterCount <= maxCharacters)
			return filename;

		const int overSize = characterCount - maxCharacters;
		return filename.substr(0, characterCount - overSize);
	}

	void loadDirectoryClips()
	{
		for (size_t i = 0; i < fileNames_.size(); i++)
		{
			AudioClip clip;
			clip.load(fileNames_[i]);
			audioClips_.push_back(clip);
		}
	}
};