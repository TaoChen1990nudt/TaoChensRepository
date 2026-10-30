#include "SamplePath.h"

#include <algorithm>

namespace atomic::samplepath
{
namespace
{
    /** `File` has no member for this — only the static `isAbsolutePath`, which
        takes the text. A default-constructed File answers false, which is what
        "this session has no home yet" means. */
    bool isAbsoluteFile (const juce::File& file)
    {
        return file != juce::File() && juce::File::isAbsolutePath (file.getFullPathName());
    }
}

//==============================================================================
juce::File asFile (const juce::String& absolutePath)
{
    if (absolutePath.isEmpty())
        return {};

    return juce::File (absolutePath);
}

//==============================================================================
juce::File resolve (const juce::String& storedPath, const juce::File& projectFile)
{
    if (storedPath.isEmpty())
        return {};

    const juce::File stored (storedPath);

    // Absolute is the whole of v26 and earlier, and it stays literal: a reader
    // that started "helpfully" relocating those would be editing a user's
    // project on open.
    if (juce::File::isAbsolutePath (storedPath) || ! isAbsoluteFile (projectFile))
        return stored;

    return projectFile.getParentDirectory().getChildFile (storedPath);
}

//==============================================================================
juce::String toStored (const juce::String& absolutePath, const juce::File& projectFile)
{
    if (absolutePath.isEmpty())
        return {};

    const juce::File target (absolutePath);

    // A relative path in the model would mean "relative to nothing in
    // particular"; leave whatever we were handed alone rather than inventing a
    // base for it.
    if (! juce::File::isAbsolutePath (absolutePath))
        return absolutePath;

    if (! isAbsoluteFile (projectFile))
        return absolutePath;

    const auto candidate = target.getRelativePathFrom (projectFile.getParentDirectory());

    if (candidate.isEmpty())
        return absolutePath;

    // The check that makes this function trustworthy. `getRelativePathFrom` is a
    // textual prefix walk: on a different drive it happily produces something
    // like `..\\..\\E:\\audio\\kick.wav`, which no reader can resolve back. The
    // resolver is the same one the document will be read with, so "it resolves
    // to the same file" is exactly the property that matters — not "it looks
    // shorter".
    if (resolve (candidate, projectFile) == target)
        return candidate;

    return absolutePath;
}

//==============================================================================
juce::File resolveExisting (const juce::String& storedPath, const juce::File& projectFile)
{
    const auto direct = resolve (storedPath, projectFile);

    if (direct == juce::File() || direct.existsAsFile())
        return direct;

    // The project folder itself, then the one subfolder a moved project
    // normally keeps its audio in. Kept to two guesses on purpose: this runs on
    // the message thread while a project opens, and a recursive search there
    // would freeze the window on a folder of samples.
    if (isAbsoluteFile (projectFile))
    {
        const auto folder = projectFile.getParentDirectory();

        for (const auto& guess : { folder.getChildFile (direct.getFileName()),
                                   folder.getChildFile ("media").getChildFile (direct.getFileName()),
                                   folder.getChildFile ("Media").getChildFile (direct.getFileName()),
                                   folder.getChildFile ("audio").getChildFile (direct.getFileName()) })
        {
            if (guess.existsAsFile())
                return guess;
        }
    }

    return direct;
}

//==============================================================================
//==============================================================================
const juce::File* FolderIndex::find (const juce::String& fileName) const
{
    const auto key = fileName.toLowerCase();

    for (const auto& entry : byName)
        if (entry.first == key)
            return &entry.second;

    return nullptr;
}

namespace
{
    void walk (const juce::File& dir, int depth, int maxDepth, FolderIndex& index)
    {
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*",
                                                                juce::File::findFilesAndDirectories))
        {
            const auto child = entry.getFile();

            if (child.isDirectory())
            {
                if (depth + 1 >= maxDepth)
                {
                    ++index.truncatedAtDepth;
                    continue;
                }

                walk (child, depth + 1, maxDepth, index);
                continue;
            }

            ++index.files;
            index.byName.emplace_back (child.getFileName().toLowerCase(), child);
        }
    }
}

FolderIndex indexFolder (const juce::File& folder, int maxDepth)
{
    FolderIndex index;

    if (folder.isDirectory())
        walk (folder, 0, juce::jmax (1, maxDepth), index);

    // First hit wins, and "first" has to mean the same thing on every run:
    // directory order is whatever the file system felt like returning.
    std::sort (index.byName.begin(), index.byName.end(),
               [] (const auto& a, const auto& b) { return a.first < b.first; });

    return index;
}

//==============================================================================
std::vector<juce::String> collectMissing (const std::vector<juce::String>& paths)
{
    std::vector<juce::String> missing;

    for (const auto& path : paths)
        if (path.isNotEmpty() && ! juce::File (path).existsAsFile())
            missing.push_back (path);

    return missing;
}

std::vector<juce::File> relinkFrom (const std::vector<juce::String>& missingPaths,
                                    const juce::File& folder)
{
    const auto index = indexFolder (folder);

    std::vector<juce::File> found;
    found.reserve (missingPaths.size());

    for (const auto& path : missingPaths)
    {
        const auto* hit = index.find (juce::File (path).getFileName());

        found.push_back (hit != nullptr ? *hit : juce::File());
    }

    return found;
}

} // namespace atomic::samplepath
