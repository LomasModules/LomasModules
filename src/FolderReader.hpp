struct FolderReader
{
	FolderReader()
	{
		displayNames_.push_back("No sample");
		fileNames_.push_back("");
	}
	
	void scanDirectory(std::string directory)
	{
		// First two entries are:
		// .
		// ..
		DIR *dir;
		struct dirent *ent;
		fileNames_.clear();
		displayNames_.clear();
		if ((dir = opendir(directory.c_str())) != NULL)
		{
			while ((ent = readdir(dir)) != NULL)
			{
				std::string fileName = ent->d_name;
				if (fileName != ".." && fileName != ".") // Dont add invalid entries.
				{
					// Only add wav files
					std::size_t found = fileName.find(".wav", fileName.length() - 5);
					if (found == std::string::npos)
						found = fileName.find(".WAV", fileName.length() - 5);

					if (found != std::string::npos)
					{
						fileNames_.push_back(directory + "\\" + fileName);
						std::string fileNameNoExt = remove_extension(fileName);
						std::string displayText = short_fileName(fileNameNoExt);
						displayNames_.push_back(displayText);
					}
				}
			}
		}
		maxFileIndex_ = fileNames_.size() - 1;
	}

	static int getFileCountInDirectory(std::string directory)
	{
		int counter = 0;
		// First two entries are:
		// .
		// ..
		DIR *dir;
		struct dirent *ent;
		if ((dir = opendir(directory.c_str())) != NULL)
		{
			while ((ent = readdir(dir)) != NULL)
			{
				std::string fileName = ent->d_name;
				if (fileName != ".." && fileName != ".") // Dont add invalid entries.
				{
					counter++;
				}
			}
		}
		return counter;
	}

	static std::string remove_extension(const std::string &filename)
	{
		size_t lastdot = filename.find_last_of(".");
		if (lastdot == std::string::npos)
			return filename;
		return filename.substr(0, lastdot);
	}

	static std::string short_fileName(const std::string &filename, int maxCharacters = 13)
	{
		const int characterCount = filename.size();

		if (characterCount <= maxCharacters)
			return filename;

		const int overSize = characterCount - maxCharacters;
		return filename.substr(0, characterCount - overSize);
	}

	std::vector<std::string> fileNames_;
	std::vector<std::string> displayNames_;
	int maxFileIndex_ = 0;
};