#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <libzippp.h>

#include "DataStream.hpp"
#include "Drillmethod.hpp"
#include "General.hpp"
#include "PadstackUsage.hpp"
#include "Parser.hpp"


namespace fs = std::filesystem;


Parser::Parser(const fs::path& aFile)
{
    mFileType = getFileTypeByExtension(aFile);

    openFile(aFile);

    // @todo parse the version somehow
    mFileFormatVersion = FileFormatVersion::Unknown;
}


Parser::~Parser()
{
    closeFile();
}


FileType Parser::getFileTypeByExtension(const fs::path& aFile) const
{
    std::string extension = aFile.extension().string();

    // Ignore case of extension.
    std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return std::tolower(c); }
        );

    FileType fileType;

    if(extension == ".brd")
    {
        fileType = FileType::brd;
    }
    else if(extension == ".mdd")
    {
        fileType = FileType::mdd;
    }
    else if(extension == ".dra")
    {
        fileType = FileType::dra;
    }
    else if(extension == ".psm")
    {
        fileType = FileType::psm;
    }
    else if(extension == ".ssm")
    {
        fileType = FileType::ssm;
    }
    else if(extension == ".fsm")
    {
        fileType = FileType::fsm;
    }
    else if(extension == ".osm")
    {
        fileType = FileType::osm;
    }
    else if(extension == ".bsm")
    {
        fileType = FileType::bsm;
    }
    else if(extension == ".pad")
    {
        fileType = FileType::pad;
    }
    else
    {
        throw std::runtime_error("Unknown file extension: " + extension);
    }

    return fileType;
}


std::vector<std::pair<std::string, size_t>> Parser::getFilesInBinary()
{
    std::vector<std::pair<std::string, size_t>> files;

    // Start at beginning of file.
    const size_t oldOffset = mDs.setCurrentOffset(0u);

    while(!mDs.isEoF())
    {
        const bool foundZip = gotoNextZip();

        if(foundZip)
        {
            const std::string fileName = fs::path{mInputFile}.replace_extension("zip").filename().string();
            files.push_back(std::make_pair(fileName, mDs.getCurrentOffset()));

            mDs.discardBytes(1u);

            // @todo Quit after fist ZIP was found as we don't handle multiple ZIPs in the binary
            //       very well. (The magic bytes of a ZIP appear a few times in the same ZIP,
            //       therefore it looks like there are a few ZIPs in the binary, though its only one).
            //       It might be a reasonable assumption, that there is only one ZIP contained in the binary.
            break;
        }
    }

    // Restore old offset.
    mDs.setCurrentOffset(oldOffset);

    return files;
}


void Parser::exportZipFiles(const fs::path& aOutputPath)
{
    const auto& files = getFilesInBinary();

    for(const auto& file : files)
    {
        const std::string& fileName   = file.first;
        const size_t&      fileOffset = file.second;

        if(fs::path(fileName).extension().string() == ".zip")
        {
            const size_t oldOffset = mDs.setCurrentOffset(fileOffset);

            exportZip(aOutputPath);

            mDs.setCurrentOffset(oldOffset);
        }
    }
}


void Parser::openFile(const fs::path& aFile)
{
    std::cout << "Opening file: " << aFile << std::endl;

    mDs = DataStream{aFile};

    if(!mDs)
    {
        throw std::runtime_error("Could not open file: " + aFile.string());
    }

    mInputFile     = aFile;
    mInputFileSize = fs::file_size(aFile);

    std::cout << "File contains " << mInputFileSize << " byte." << std::endl;
}


void Parser::closeFile()
{
    std::cout << "Closing file: " << mInputFile << std::endl;

    mDs.close();

    mInputFile.clear();
    mInputFileSize = 0u;
}


bool Parser::gotoNextZip()
{
    while(!mDs.isEoF())
    {
        // @todo The magic number appreas for each file in the ZIP. This leads to parsing the ZIP a few times right from
        //       the middle of the file, which fails. How can we differentiate the "real" start of the ZIP?
        const std::vector<uint8_t> magicNumberZip{0x50, 0x4b, 0x03, 0x04};
        if(magicNumberZip == mDs.peek(magicNumberZip.size())) // Compare with ZIP magic number
        {
            // Found ZIP.
            return true;
        }

        mDs.readUint8();
    }

    return false;
}


void Parser::exportZip(const fs::path& aOutputPath, size_t aComprZipSize)
{
    const size_t zipBufferLen = (aComprZipSize == 0u) ? (mInputFileSize - mDs.getCurrentOffset()) : aComprZipSize;
    std::unique_ptr<char[]> zipBuffer = std::make_unique<char[]>(zipBufferLen);

    mDs.read(zipBuffer.get(), zipBufferLen);

    // @todo Implement check for magic bytes
    // const std::vector<uint8_t> magicNumberZip{0x50, 0x4b, 0x03, 0x04};

    libzippp::ZipArchive& zf = *(libzippp::ZipArchive::fromBuffer(zipBuffer.get(), zipBufferLen));

    zf.open(libzippp::ZipArchive::ReadOnly);

    // Export ZIP container itself.
    {
        fs::path fileName = fs::path{mInputFile}.replace_extension("zip").filename();

        // If ZIP container consists only of 1 file, use that name instead.
        if(zf.getEntriesCount() == 1u)
        {
            const auto& entry = zf.getEntries().front();

            if(entry.isFile())
            {
                fileName = fs::path{entry.getName()}.replace_extension("zip").filename();
            }
        }

        fs::path filePath = aOutputPath / mInputFile.filename() / fileName;
        fs::create_directories(filePath.parent_path());

        std::cout << "Extract ZIP file: " << filePath.string() << std::endl;

        // Note: In case the ZIP size is not specified we use a buffer that is greater or equal the ZIP size.
        // Writing data after the end of the ZIP is not great but does no harm. The ZIP can still be opened correctly.

        std::ofstream ofs;
        ofs.open(filePath, std::ofstream::out | std::ofstream::binary);
        ofs.write(zipBuffer.get(), zipBufferLen);
        ofs.close();
    }

    // Export content of ZIP container.
    for(const auto& entry : zf.getEntries())
    {
        fs::path basePath = aOutputPath / mInputFile.filename();

        const fs::path entryPath = entry.getName();

        fs::create_directories((basePath / entryPath).parent_path());

        if(entry.isFile())
        {
            const libzippp_uint64 fileSize = entry.getSize();

            const void* binaryData = entry.readAsBinary();

            if(nullptr == binaryData)
            {
                throw std::runtime_error("Could not read ZIP as binary!");
            }

            fs::path filePath = basePath / entryPath;

            std::cout << "Extract from ZIP, " << fileSize << " Byte file: " << filePath.string() << std::endl;

            // Note: In case the ZIP size is not specified we use a buffer that is greater or equal the ZIP size.
            // Writing data after the end of the ZIP is not great but does no harm. The ZIP can still be opened correctly.

            // Write content of ZIP container.
            std::ofstream ofs;
            ofs.open(filePath, std::ofstream::out | std::ofstream::binary);
            ofs.write(static_cast<const char*>(binaryData), fileSize);
            ofs.close();

            // @todo figure out how to delete this. See documentation of the function that sets binaryData.
            delete static_cast<const uint8_t*>(binaryData);
            binaryData = nullptr;
        }
    }

    zf.close();
}


Pad Parser::readPad()
{
    Pad pad;

    pad.setFigure(mDs.readUint16());
    pad.setSpecialCorners(mDs.readUint16());

    pad.setNsides(mDs.readUint32());

    pad.setWidth(mDs.readInt32());
    pad.setHeight(mDs.readInt32());

    pad.setCorner(mDs.readInt32());

    pad.setOffsetX(mDs.readInt32());
    pad.setOffsetY(mDs.readInt32());

    mDs.printUnknownData(std::cout, 4, "pad data - 0");

    // @todo set only for SHAPE_SYMBOL
    const uint32_t idxShapeSymolStr = mDs.readUint32();
    std::cout << "idxShapeSymolStr = " << idxShapeSymolStr << std::endl;

    return pad;
}


enum class Type
{
    REGULAR_PAD,
    THERMAL_PAD,
    ANTIPAD_PAD,
    KEEPOUT,
    USER_MASK,
    UNKNOWN // @todo remove
};

enum class Layer
{
    DEFAULT_INTERNAL,
    END_LAYER,
    ADJACENT_KEEPOUT,
    TOP_SOLDER_MASK_PAD,
    BOTTOM_SOLDER_MASK_PAD,
    TOP_PASTE_MASK_PAD,
    BOTTOM_PASTE_MASK_PAD,
    TOP_FILM_MASK_PAD,
    BOTTOM_FILM_MASK_PAD,
    TOP_COVERLAY_PAD,
    BOTTOM_COVERLAY_PAD,
    BACKDRILL_SOLDERMASK,
    UNKNOWN // @todo remove
};

struct padTypeLayer
{
    Type  type;
    Layer layer;
};

std::vector<padTypeLayer> layerLst = {
    // @todo figure out. This one is weird.
    //       Maybe its not a pad but some other info?
    { Type::UNKNOWN,     Layer::UNKNOWN                }, // Index  0

    { Type::ANTIPAD_PAD, Layer::DEFAULT_INTERNAL       }, // Index  1
    { Type::THERMAL_PAD, Layer::DEFAULT_INTERNAL       }, // Index  2
    { Type::REGULAR_PAD, Layer::DEFAULT_INTERNAL       }, // Index  3
    { Type::KEEPOUT,     Layer::DEFAULT_INTERNAL       }, // Index  4

    // @todo Probably Begin layer
    //       Maybe it must also be swapped with the upper INTERNAL layers
    { Type::UNKNOWN,     Layer::UNKNOWN                }, // Index  5
    { Type::UNKNOWN,     Layer::UNKNOWN                }, // Index  6
    { Type::UNKNOWN,     Layer::UNKNOWN                }, // Index  7
    { Type::UNKNOWN,     Layer::UNKNOWN                }, // Index  8

    { Type::REGULAR_PAD, Layer::BACKDRILL_SOLDERMASK   }, // Index  9

    { Type::REGULAR_PAD, Layer::TOP_COVERLAY_PAD       }, // Index 10
    { Type::REGULAR_PAD, Layer::BOTTOM_COVERLAY_PAD    }, // Index 11

    // @todo figure out
    { Type::UNKNOWN,     Layer::UNKNOWN                }, // Index 12
    { Type::UNKNOWN,     Layer::UNKNOWN                }, // Index 13

    { Type::REGULAR_PAD, Layer::TOP_SOLDER_MASK_PAD    }, // Index 14
    { Type::REGULAR_PAD, Layer::BOTTOM_SOLDER_MASK_PAD }, // Index 15

    { Type::REGULAR_PAD, Layer::TOP_PASTE_MASK_PAD     }, // Index 16
    { Type::REGULAR_PAD, Layer::BOTTOM_PASTE_MASK_PAD  }, // Index 17

    { Type::REGULAR_PAD, Layer::TOP_FILM_MASK_PAD      }, // Index 18
    { Type::REGULAR_PAD, Layer::BOTTOM_FILM_MASK_PAD   }, // Index 19

    { Type::KEEPOUT,     Layer::ADJACENT_KEEPOUT       }, // Index 20

    { Type::ANTIPAD_PAD, Layer::END_LAYER              }, // Index 21
    { Type::THERMAL_PAD, Layer::END_LAYER              }, // Index 22
    { Type::REGULAR_PAD, Layer::END_LAYER              }, // Index 23
    { Type::KEEPOUT,     Layer::END_LAYER              }  // Index 24
};


PadFile Parser::readPadFile(unknownParam uparam)
{
    PadFile padFile;

    // Specifies how many seconds are allowed to pass
    // until two contiguous sections are generated.
    const double maxTimeDiff = 2.0;

    mDs.printUnknownData(std::cout, 8, "unknown - 0");

    // mDs.assumeData({0x00, 0x05, 0x14, 0x00, 0x03, 0x00, 0x00, 0x00}, "Start Sequence - 0");
    mDs.assumeData({0x04, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}, "Start Sequence - 1");

    mDs.printUnknownData(std::cout, 36, "unknown - 1");

    // @todo This relates maybe to the standard layers in the padstack as we
    // have 25 of them and 1 of them is just rubbish, and can be ignored
    // => We have 24 standard packstack layers
    for(size_t i = 0u; i < 24u; ++i)
    {
        const uint32_t some_idx = mDs.readUint32();
        const uint32_t some_val = mDs.readUint32();

        std::cout << "some_idx = " << some_idx << "; some_val = " << some_val << std::endl;
    }

    // @todo probably always number 1 and represents idx = 1?
    mDs.printUnknownData(std::cout, 4, "unkown - 2");

    padFile.swVersion = mDs.readStrZeroTermBlock(60u);
    // std::cout << "swVersion = " << padFile.swVersion << std::endl;

    mDs.printUnknownData(std::cout, 58, "unknown - 3");

    padFile.accuracy = mDs.readUint16();

    const int32_t some_val0 = mDs.readInt32();
    const int32_t some_val1 = mDs.readInt32();
    const int32_t some_val2 = mDs.readInt32();
    const int32_t some_val3 = mDs.readInt32();

    std::cout << "some_val0 = " << some_val0 << std::endl;
    std::cout << "some_val1 = " << some_val1 << std::endl;
    std::cout << "some_val2 = " << some_val2 << std::endl;
    std::cout << "some_val3 = " << some_val3 << std::endl;

    padFile.unit = ToUnits(mDs.readUint16());

    mDs.printUnknownData(std::cout, 227, "unknown - 4");

    const uint16_t additionalStr = mDs.readUint16();

    mDs.printUnknownData(std::cout, 8, "unknown - 5");

    mDs.assumeZero(449, "unknown - 6");

    mDs.printUnknownData(std::cout, 36, "unknown - 7");

    for(size_t i = 0u; i < 20u; ++i)
    {
        const uint32_t sth0 = mDs.readUint32();
        const uint32_t sth1 = mDs.readUint32();

        std::cout << "sth0 = " << sth0 << "; sth1 = " << sth1 << std::endl;
    }

    mDs.printUnknownData(std::cout, 20, "unknown - 8");

    mDs.assumeZero(248, "unknown - 9");

    for(size_t i = 0u; i < 7u + additionalStr + uparam.additionalStr2; ++i)
    {
        const uint32_t    idx = mDs.readUint32();
        const std::string str = mDs.readStrZeroTerm4BytePad();

        padFile.idxStrPairLst.push_back(std::make_pair(idx, str));

        std::cout << "idxStrPairLst[" << std::to_string(padFile.idxStrPairLst.size()) << "] : "
                  << "idx = " << idx << "; str = " << str << std::endl;
    }

    // @todo Still unknown whether there is a string stored or just some
    //       other info
    {
        const uint32_t idx    = mDs.readUint32();

        mDs.printUnknownData(std::cout, 4, "unknown - 10");

        const std::string str = "";

        padFile.idxStrPairLst.push_back(std::make_pair(idx, str));

        std::cout << "idxStrPairLst[" << std::to_string(padFile.idxStrPairLst.size()) << "] : "
                  << "idx = " << idx << "; str = " << str << std::endl;
    }

    // drillinfo

    // Contains drilltoolsize
    {
        const uint32_t idx    = mDs.readUint32();
        const std::string str = mDs.readStrZeroTerm4BytePad();

        padFile.drilltoolsize = str;

        padFile.idxStrPairLst.push_back(std::make_pair(idx, str));

        std::cout << "idxStrPairLst[" << std::to_string(padFile.idxStrPairLst.size()) << "] : "
                  << "idx = " << idx << "; str = " << str << std::endl;
    }

    // @todo Maybe Add the above two indicies and strings to the loop and increase the
    // limit from 7 to 9

    // @todo figure out, when it is set. It think it is somehow related to
    //       drills, backdrills or multiple drill rows/columns
    if(uparam.unknownFlag)
    {
        mDs.printUnknownData(std::cout, 8, "unknown - 11");
    }

    mDs.printUnknownData(std::cout, 4, "unknown - 12");

    padFile.strIdxPadName       = mDs.readUint32();
    padFile.idxUnknown          = mDs.readUint32();
    padFile.strIdxDrillToolSize = mDs.readUint32(); // Is 0 when no DrillToolSize is specified (empty string)

    // std::cout << "strIdxPadName       = " << padFile.strIdxPadName       << std::endl;
    // std::cout << "idxUnknown          = " << padFile.idxUnknown          << std::endl;
    // std::cout << "strIdxDrillToolSize = " << padFile.strIdxDrillToolSize << std::endl;

    mDs.printUnknownData(std::cout, 5, "unknown - 13");

    padFile.drillmethod = ToDrillmethod(mDs.readUint8());
    // std::cout << "drillmethod = " << padFile.drillmethod << std::endl;

    // Bit 0 = @todo Unknown
    // Bit 3 = Multiple Drills @todo verify this (number of column/row drills)
    // Bit 4 = Staggered Drills
    // Bit 5 = Plated Drill Holes
    const uint8_t bit_field = mDs.readUint8();
    std::cout << "unknown bit_field: " << std::to_string(bit_field) << std::endl;
    padFile.staggeredDrills = static_cast<bool>(bit_field & 0x10);
    padFile.plated          = static_cast<bool>(bit_field & 0x20);
    // std::cout << "plated = " << padFile.plated << std::endl;

    // Check for unknown bits that are set
    if(bit_field & ~0x30)
    {
        // throw std::runtime_error("Unknown bit in bit_field set! 0x" + ToHex(bit_field, 2));
    }

    mDs.printUnknownData(std::cout, 2, "unknown - 14");

    // Bit 0 = Not suppress not connected internal pads
    const uint8_t bit_field2 = mDs.readUint8();
    std::cout << "unknown bit_field2: " << std::to_string(bit_field2) << std::endl;
    padFile.not_suppress_nc_internal_pads = static_cast<bool>(bit_field2 & 0x01);
    padFile.isPolyVia                     = static_cast<bool>(bit_field2 & 0x02);

    // Check for unknown bits that are set
    if(bit_field2 & ~0x03)
    {
        throw std::runtime_error("Unknown bit in bit_field2 set! 0x" + ToHex(bit_field2, 2));
    }

    mDs.printUnknownData(std::cout, 4, "unknown - 15");

    // @todo unknown
    const uint16_t type_bitfield = mDs.readUint16();
    // std::cout << "type_bitfield = " << type_bitfield << std::endl;

    // @todo this is completly wrong! padstackusage is stored somewhere else but not here!
    padFile.padstackusage = ToPadstackUsage(mDs.readUint16());
    // std::cout << "padstackusage = " << padFile.padstackusage << std::endl;

    // multidrill

    padFile.drill_rows    = mDs.readUint16();
    padFile.drill_columns = mDs.readUint16();
    // std::cout << "rows = " << padFile.drill_rows << "; columns = " << padFile.drill_columns << std::endl;

    uint8_t lock_layer_span = mDs.readUint8();
    padFile.lock_layer_span = static_cast<bool>(lock_layer_span);

    mDs.printData(std::cout, {lock_layer_span});

    if(lock_layer_span > 1)
    {
        throw std::runtime_error("Epected boolean value!");
    }

    mDs.printUnknownData(std::cout, 1, "unknown - 16");

    padFile.offsetX = mDs.readInt32();
    padFile.offsetY = mDs.readInt32();
    // std::cout << "offsetX = " << padFile.offsetX << "; offsetY = " << padFile.offsetY << std::endl;

    padFile.clearance_columns = mDs.readUint32();
    padFile.clearance_rows    = mDs.readUint32();
    // std::cout << "clearance_columns = " << padFile.clearance_columns << "; clearance_rows = " << padFile.clearance_rows << std::endl;

    padFile.finished_size = mDs.readInt32();
    // std::cout << "finished_size = " << padFile.finished_size << std::endl;

    mDs.printUnknownData(std::cout, 0, "unknown - 17");

    padFile.positivetolerance = mDs.readInt32();
    padFile.negativetolerance = mDs.readInt32();
    // std::cout << "positivetolerance = " << padFile.positivetolerance << "; negativetolerance = " << padFile.negativetolerance << std::endl;

    mDs.printUnknownData(std::cout, 16, "unknown - 18");

    padFile.width  = mDs.readUint32();
    padFile.height = mDs.readUint32();
    // std::cout << "width = " << padFile.width << "; height = " << padFile.height << std::endl;

    padFile.figure = ToFigure(mDs.readUint32());

    padFile.characters = mDs.readStrZeroTerm4BytePad();
    // std::cout << "characters = " << padFile.characters << std::endl;

    mDs.printUnknownData(std::cout, 12, "unknown - 19");

    // backdrill

    padFile.back_drill_figure_width  = mDs.readUint32();
    padFile.back_drill_figure_height = mDs.readUint32();

    padFile.back_drill_figure = ToFigure(mDs.readUint32());

    padFile.back_drill_characters = mDs.readStrZeroTerm4BytePad();

    // counterboresink

    padFile.counter_drill_diameter = mDs.readInt32();
    padFile.counter_drill_positivetolerance = mDs.readInt32();
    padFile.counter_drill_negativetolerance = mDs.readInt32();

    padFile.counterangle = mDs.readInt32();
    // std::cout << "counterangle = " << std::to_string(padFile.counterangle / 10000u) << std::endl;

    mDs.printUnknownData(std::cout, 8, "Something with counterdepth");

    mDs.printUnknownData(std::cout, 32, "unknown - 20");

    for(size_t i = 0u; i < 25u; ++i)
    {
        // std::cout << "i = " << i << std::endl;
        // @todo print layer and type
        padFile.genericLayers.push_back(readPad());
        // std::cout << pad << std::endl;
    }

    std::cout << "Start user layers" << std::endl;

    for(size_t i = 0u; i < uparam.numUserLayers; ++i)
    {
        // std::cout << "i = " << i << std::endl;
        const uint32_t idxLayerName = mDs.readUint32();
        // std::cout << "idxLayerName = " << idxLayerName << std::endl;

        Pad pad = readPad();
        // std::cout << pad << std::endl;
    }

    std::cout << "Exit loop";

    mDs.printUnknownData(std::cout, 56, "unknown - 21");

    padFile.dateTime1 = ToTime(mDs.readUint32());
    // std::cout << "dateTime1 = " << DateTimeToStr(padFile.dateTime1) << std::endl;

    mDs.printUnknownData(std::cout, 18, "unknown - 22");

    // @todo not sure about this one
    const size_t usernameLen = mDs.readUint32();

    mDs.printUnknownData(std::cout, 2, "unknown - 23");

    padFile.username = mDs.readStrZeroTerm4BytePad();
    // std::cout << "username = " << padFile.username << std::endl;

    if(padFile.username.size() + 1 != usernameLen) // +1 for terminating zero byte.
    {
        throw std::runtime_error("Expected different text length!");
    }

    mDs.printUnknownData(std::cout, 32, "unknown - 24");

    const size_t someTxtLen = mDs.readUint32();

    const std::string quickViewText = mDs.readStrZeroTermBlock(128);

    expectStr(quickViewText, "_QuickViewText");

    const std::string quickViewsRef = "_QUICKVIEWS";
    std::string quickViews = mDs.readStrZeroTermBlock(32);

    expectStr(quickViews, quickViewsRef);

    const auto sanityCheckSectionTimeDiff = [] (time_t aStartTime, time_t aEndTime, double aMaxTimeDiff) -> void
        {
            // Sanity check that previous sections were created within maxSecDiff seconds.
            if(std::difftime(aEndTime, aStartTime) > aMaxTimeDiff)
            {
                throw std::runtime_error("Difference in generation time must be lower than "
                    + std::to_string(aMaxTimeDiff) + " seconds but date and time is start "
                    + DateTimeToStr(aStartTime) + " and end " + DateTimeToStr(aEndTime) + "!");
            }
        };


    padFile.dateTime2 = ToTime(mDs.readUint32());

    sanityCheckSectionTimeDiff(padFile.dateTime1, padFile.dateTime2, maxTimeDiff);

    mDs.assumeZero(8, "unknown - 23");

    const std::string someTxt = mDs.readStrZeroTerm4BytePad();

    if(someTxt.size() + 1 != someTxtLen) // +1 for terminating zero byte.
    {
        throw std::runtime_error("Expected different text length!");
    }

    // std::cout << "someTxt = " << someTxt << std::endl;

    mDs.printUnknownData(std::cout, 8, "unknown - 25");

    const std::string quickViewGraph = mDs.readStrZeroTermBlock(128);

    expectStr(quickViewGraph, "_QuickViewGraph");

    quickViews = mDs.readStrZeroTermBlock(32);

    expectStr(quickViews, quickViewsRef);

    padFile.dateTime3 = ToTime(mDs.readUint32());

    sanityCheckSectionTimeDiff(padFile.dateTime2, padFile.dateTime3, maxTimeDiff);

    mDs.printUnknownData(std::cout, 36, "unknown - 26");

    const uint32_t zipSize = mDs.readUint32();

    const std::string quickViewJson = mDs.readStrZeroTermBlock(128);

    expectStr(quickViewJson, "_QuickViewJson");

    quickViews = mDs.readStrZeroTermBlock(32);

    expectStr(quickViews, quickViewsRef);

    padFile.dateTime4 = ToTime(mDs.readUint32());

    sanityCheckSectionTimeDiff(padFile.dateTime3, padFile.dateTime4, maxTimeDiff);

    mDs.printUnknownData(std::cout, 8, "unknown - 27");

    exportZip(fs::temp_directory_path() / "OpenAllegroParser", zipSize);

    mDs.padTo4ByteBoundary();

    mDs.printUnknownData(std::cout, 8, "unknown - 28");

    const std::string newDbFeatures = mDs.readStrZeroTermBlock(160);

    expectStr(newDbFeatures, "NEW_DB_FEATURES");

    padFile.dateTime5 = ToTime(mDs.readUint32());

    sanityCheckSectionTimeDiff(padFile.dateTime4, padFile.dateTime5, maxTimeDiff);

    mDs.printUnknownData(std::cout, 20, "unknown - 29");

    const std::string allegroDesignWasLastSaved = mDs.readStrZeroTermBlock(160);

    expectStr(allegroDesignWasLastSaved, "ALLEGRO_DESIGN_WAS_LAST_SAVED");

    padFile.dateTime6 = ToTime(mDs.readUint32());

    sanityCheckSectionTimeDiff(padFile.dateTime5, padFile.dateTime6, maxTimeDiff);

    mDs.assumeZero(8, "unknown - 30");

    padFile.programAndVersion = mDs.readStrZeroTerm4BytePad();
    // std::cout << "programAndVersion = " << padFile.programAndVersion << std::endl;

    if(!mDs.isEoF())
    {
        throw std::runtime_error("Expected EoF!");
    }

    return padFile;
}