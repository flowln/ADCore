/* NDFileNetCDF.cpp
 * Writes NDArrays to netCDF files.
 *
 * Mark Rivers
 * April 17, 2008
 */
 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netcdf.h>

#include <epicsStdio.h>
#include <iocsh.h>
#include <epicsExport.h>

#include "NDFileNetCDF.h"

static const char *driverName = "NDFileNetCDF";

/* Handle errors by printing an error message and exiting with a
 * non-zero status. */
#define ERR(e) {asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, \
                "%s:%s error=%s\n", \
                driverName, functionName, nc_strerror(e)); \
                return(asynError);}

/* NDArray string attributes can be of any length, but netCDF requires a fixed maximum length
 * which we define here. */
#define MAX_ATTRIBUTE_STRING_SIZE 256

/** Opens a netCDF file.  
  * In write mode if NDFileModeMultiple is set then the first dimension is set to NC_UNLIMITED to allow 
  * multiple arrays to be written to the same file.
  * NOTE: Does not currently support NDFileModeRead or NDFileModeAppend.
  * \param[in] fileName  Absolute path name of the file to open.
  * \param[in] openMode Bit mask with one of the access mode bits NDFileModeRead, NDFileModeWrite, NDFileModeAppend.
  *           May also have the bit NDFileModeMultiple set if the file is to be opened to write or read multiple 
  *           NDArrays into a single file.
  * \param[in] pArray Pointer to an NDArray; this array is used to determine the header information and data 
  *           structure for the file. */
asynStatus NDFileNetCDF::openFile(const char *fileName, NDFileOpenMode_t openMode, NDArray *pArray)
{
    /* When we create netCDF variables and dimensions, we get back an
     * ID for each one. */
    int dimIds[ND_ARRAY_MAX_DIMS+1];
    int stringDimIds[2];
    int size[ND_ARRAY_MAX_DIMS], offset[ND_ARRAY_MAX_DIMS];
    int binning[ND_ARRAY_MAX_DIMS], reverse[ND_ARRAY_MAX_DIMS];
    char dimName[25];
    int dim0;
    int retval;
    nc_type ncType=NC_NAT;
    int i, j;
    NDAttribute *pAttribute;
    char name[MAX_ATTRIBUTE_STRING_SIZE];
    char description[MAX_ATTRIBUTE_STRING_SIZE];
    char tempString[MAX_ATTRIBUTE_STRING_SIZE];
    NDAttrDataType_t attrDataType;
    size_t attrSize;
    int numAttributes, attrCount;
    double fileVersion;
    static const char *functionName = "openFile";

    /* We don't support reading yet */    
    if (openMode & NDFileModeRead) return(asynError);
    
    /* We don't support opening an existing file for appending yet */    
    if (openMode & NDFileModeAppend) return(asynError);

    /* Set the next record in the file to 0 */
    this->nextRecord = 0;

    /* Create the file. The NC_CLOBBER parameter tells netCDF to
     * overwrite this file, if it already exists.*/
    if ((retval = nc_create(fileName, NC_CLOBBER, &this->ncId)))
        ERR(retval);

    /* Create global attribute for the data type because netCDF does not
     * distinguish signed and unsigned.  Readers can use this to know how to treat
     * integer data. */
    if ((retval = nc_put_att_int(this->ncId, NC_GLOBAL, "dataType", 
                                 NC_INT, 1, (const int*)&pArray->dataType)))
        ERR(retval);

    /* Create global attribute with NDNetCDFFileVersion so readers can handle changes
     * in file contents */
    fileVersion = NDNetCDFFileVersion;
    if ((retval = nc_put_att_double(this->ncId, NC_GLOBAL, "NDNetCDFFileVersion",
                                 NC_DOUBLE, 1, &fileVersion)))
        ERR(retval);

    /* Create global attribute for number of dimensions and dimensions
     * in each NDArray.
     * This is redundant with information netCDF puts in, but the netCDF
     * info includes the number of arrays in the file.  This can make it
     * easier to write readers */
    if ((retval = nc_put_att_int(this->ncId, NC_GLOBAL, "numArrayDims", 
                                 NC_INT, 1, &pArray->ndims)))
        ERR(retval);

    /* Define the dimensions. NetCDF will hand back an ID for each.
     * netCDF has the first dimension changing slowest, opposite of NDArrayBuff
     * convention. 
     * We make the first dimension the number of arrays in the file.  This is either
     * 1 or NC_UNLIMITED */
    dim0 = 1;
    if (openMode & NDFileModeMultiple) dim0 = NC_UNLIMITED;
    if ((retval = nc_def_dim(this->ncId, "numArrays", dim0, &dimIds[0])))
        ERR(retval);

    /* The next dimensions are the dimensions of the data in reversed order */
    for (i=0; i<pArray->ndims; i++) {
        j = pArray->ndims - i - 1;
        sprintf(dimName, "dim%d", i);
        if ((retval = nc_def_dim(this->ncId, dimName, pArray->dims[j].size, &dimIds[i+1])))
            ERR(retval);
        size[i]    = pArray->dims[i].size;
        offset[i]  = pArray->dims[i].offset;
        binning[i] = pArray->dims[i].binning;
        reverse[i]  = pArray->dims[i].reverse;
    }

    /* String attributes are special.  The first dimension is the number of arrays, the second is the string size */
    stringDimIds[0] = dimIds[0];
    if ((retval = nc_def_dim(this->ncId, "attrStringSize", MAX_ATTRIBUTE_STRING_SIZE, &stringDimIds[1])))
        ERR(retval);

    /* Create global attribute for information about the dimensions */
    if ((retval = nc_put_att_int(this->ncId, NC_GLOBAL, "dimSize", 
                                 NC_INT, pArray->ndims, size)))
        ERR(retval);
    if ((retval = nc_put_att_int(this->ncId, NC_GLOBAL, "dimOffset", 
                                 NC_INT, pArray->ndims, offset)))
        ERR(retval);
    if ((retval = nc_put_att_int(this->ncId, NC_GLOBAL, "dimBinning", 
                                 NC_INT, pArray->ndims, binning)))
        ERR(retval);
    if ((retval = nc_put_att_int(this->ncId, NC_GLOBAL, "dimReverse", 
                                 NC_INT, pArray->ndims, reverse)))
        ERR(retval);

    /* Convert from NDArray data types to netCDF data types */
    switch (pArray->dataType) {
        case NDInt8:
        case NDUInt8:
            ncType = NC_BYTE;
            break;
        case NDInt16:
        case NDUInt16:
            ncType = NC_SHORT;
            break;
        case NDInt32:
        case NDUInt32:
            ncType = NC_INT;
            break;
        case NDFloat32:
            ncType = NC_FLOAT;
            break;
        case NDFloat64:
            ncType = NC_DOUBLE;
            break;
    }

    /* Define the unique data variable. */
    if ((retval = nc_def_var(this->ncId, "uniqueId", NC_INT, 1, 
                 &dimIds[0], &this->uniqueIdId)))
        ERR(retval);

    /* Define the timestamp data variable. */
    if ((retval = nc_def_var(this->ncId, "timeStamp", NC_DOUBLE, 1, 
                 &dimIds[0], &this->timeStampId)))
        ERR(retval);

    /* Define the array data variable. */
    if ((retval = nc_def_var(this->ncId, "array_data", ncType, pArray->ndims+1,
                 dimIds, &this->arrayDataId)))
        ERR(retval);

    /* Create a variable for each attribute in the array */
    free(this->pAttributeId);
    numAttributes = pArray->numAttributes();
    attrCount = 0;
    this->pAttributeId = (int *)calloc(numAttributes, sizeof(int));
    pAttribute = pArray->nextAttribute(NULL);
    while (pAttribute) {
        pAttribute->getValueInfo(&attrDataType, &attrSize);
        pAttribute->getName(name, sizeof(name));
        epicsSnprintf(tempString, sizeof(tempString), "%s_DataType", name);
        if ((retval = nc_put_att_int(this->ncId, NC_GLOBAL, tempString, 
                                     NC_INT, 1, (const int*)&attrDataType)))
            ERR(retval);
        pAttribute->getDescription(description, sizeof(description));
        epicsSnprintf(tempString, sizeof(tempString), "%s_Description", name);
        if ((retval = nc_put_att_text(this->ncId, NC_GLOBAL, tempString, 
                                     strlen(description), description)))
            ERR(retval);
        switch (attrDataType) {
            case NDAttrInt8:
            case NDAttrUInt8:
                ncType = NC_BYTE;
                break;
            case NDAttrInt16:
            case NDAttrUInt16:
                ncType = NC_SHORT;
                break;
            case NDAttrInt32:
            case NDAttrUInt32:
                ncType = NC_INT;
                break;
            case NDAttrFloat32:
                ncType = NC_FLOAT;
                break;
            case NDAttrFloat64:
                ncType = NC_DOUBLE;
                break;
            case NDAttrString:
                ncType = NC_CHAR;
                break;
            case NDAttrUndefined:
                return(asynError);
                break;
        }
        if (attrDataType == NDAttrString) {
            if ((retval = nc_def_var(this->ncId, name, ncType, 2,
                    stringDimIds, &this->pAttributeId[attrCount++])))
                    ERR(retval);
        } else {
            if ((retval = nc_def_var(this->ncId, name, ncType, 1,
                    &dimIds[0], &this->pAttributeId[attrCount++])))
                    ERR(retval);
        }
        pAttribute = pArray->nextAttribute(pAttribute);
    }

    /* End define mode. This tells netCDF we are done defining
     * metadata. */
    if ((retval = nc_enddef(this->ncId)))
        ERR(retval);
    return(asynSuccess);
}


/** Writes NDArray data to a netCDF file.
  * \param[in] pArray Pointer to an NDArray to write to the file. This function can be called multiple
  *           times between the call to openFile and closeFile if
  *           NDFileModeMultiple was set in openMode in the call to NDFileNetCDF::openFile. */ 
asynStatus NDFileNetCDF::writeFile(NDArray *pArray)
{       
    int retval;
    size_t stringCount[2];
    size_t start[ND_ARRAY_MAX_DIMS+1], count[ND_ARRAY_MAX_DIMS+1];
    int attrId;
    NDAttrValue attrVal;
    int i, j;
    NDAttribute *pAttribute;
    NDAttrDataType_t attrDataType;
    size_t attrSize;
    int attrCount;
    char attrString[MAX_ATTRIBUTE_STRING_SIZE];
    static const char *functionName = "writeFile";

    count[0] = 1;
    start[0] = this->nextRecord;
    for (i=0; i<pArray->ndims; i++) {
        j = pArray->ndims - i - 1;
        count[i+1] = pArray->dims[j].size;
        start[i+1] = 0;
    }
 
    /* Write the data to the file. */
    if ((retval = nc_put_vara_int(this->ncId, this->uniqueIdId, start, count, &pArray->uniqueId)))
                ERR(retval);
    if ((retval = nc_put_vara_double(this->ncId, this->timeStampId, start, count, &pArray->timeStamp)))
                ERR(retval);

    switch (pArray->dataType) {
        case NDInt8:
        case NDUInt8:
            if ((retval = nc_put_vara_schar(this->ncId, this->arrayDataId, start, count, (signed char*)pArray->pData)))
                ERR(retval);
            break;
        case NDInt16:
        case NDUInt16:
            if ((retval = nc_put_vara_short(this->ncId, this->arrayDataId, start, count, (short *)pArray->pData)))
                ERR(retval);
            break;
        case NDInt32:
        case NDUInt32:
            if ((retval = nc_put_vara_int(this->ncId, this->arrayDataId, start, count, (int *)pArray->pData)))
                ERR(retval);
            break;
        case NDFloat32:
            if ((retval = nc_put_vara_float(this->ncId, this->arrayDataId, start, count, (float *)pArray->pData)))
                ERR(retval);
            break;
        case NDFloat64:
            if ((retval = nc_put_vara_double(this->ncId, this->arrayDataId, start, count, (double *)pArray->pData)))
                ERR(retval);
            break;
    }
    /* Write the attributes.  Loop through the list of attributes.  These must not have changed since define time! */
    pAttribute = pArray->nextAttribute(NULL);
    attrCount = 0;
    while (pAttribute) {
        pAttribute->getValueInfo(&attrDataType, &attrSize);
        attrId = this->pAttributeId[attrCount++];
        switch (attrDataType) {
            case NDAttrInt8:
            case NDAttrUInt8:
                pAttribute->getValue(attrDataType, &attrVal.i8);
                if ((retval = nc_put_vara_schar(this->ncId, attrId, start, count, (signed char*)&attrVal.i8)))
                    ERR(retval);
                break;
            case NDAttrInt16:
            case NDAttrUInt16:
                pAttribute->getValue(attrDataType, &attrVal.i16);
                if ((retval = nc_put_vara_short(this->ncId, attrId, start, count, (short *)&attrVal.i16)))
                    ERR(retval);
                break;
            case NDAttrInt32:
            case NDAttrUInt32:
                pAttribute->getValue(attrDataType, &attrVal.i32);
                if ((retval = nc_put_vara_int(this->ncId, attrId, start, count, (int *)&attrVal.i32)))
                    ERR(retval);
                break;
            case NDAttrFloat32:
                pAttribute->getValue(attrDataType, &attrVal.f32);
                if ((retval = nc_put_vara_float(this->ncId, attrId, start, count, (float *)&attrVal.f32)))
                    ERR(retval);
                break;
            case NDAttrFloat64:
                pAttribute->getValue(attrDataType, &attrVal.f64);
                if ((retval = nc_put_vara_double(this->ncId, attrId, start, count, (double *)&attrVal.f64)))
                    ERR(retval);
                break;
            case NDAttrString:
                pAttribute->getValue(attrDataType, attrString, sizeof(attrString));
                stringCount[0] = 1;
                stringCount[1] = strlen(attrString);
                if ((retval = nc_put_vara_text(this->ncId, attrId, start, stringCount, attrString)))
                    ERR(retval);
                break;
            case NDAttrUndefined:
                return(asynError);
                break;
        }
        pAttribute = pArray->nextAttribute(pAttribute);
    }
    this->nextRecord++;
    return(asynSuccess);
}

/** Read NDArray data from a netCDF file; NOTE: not implemented yet.
  * \param[in] pArray Pointer to the address of an NDArray to read the data into.  */ 
asynStatus NDFileNetCDF::readFile(NDArray **pArray)
{
    //static const char *functionName = "readFile";

    return asynError;
}


/** Closes the netCDF file opened with NDFileNetCDF::openFile */ 
asynStatus NDFileNetCDF::closeFile()
{
    int retval;
    static const char *functionName = "closeFile";

    if ((retval = nc_close(this->ncId)))
        ERR(retval);
    return asynSuccess;
}


/** Constructor for NDFileNetCDF; parameters are identical to those for NDPluginFile::NDPluginFile,
    and are passed directly to that base class constructor.
  * After calling the base class constructor this method sets NDPluginFile::supportsMultipleArrays=1.
  */
NDFileNetCDF::NDFileNetCDF(const char *portName, int queueSize, int blockingCallbacks, 
                           const char *NDArrayPort, int NDArrayAddr,
                           int priority, int stackSize)
    /* Invoke the base class constructor.
     * We allocate 1 NDArray of unlimited size in the NDArray pool.
     * This driver can block (because writing a file can be slow), and it is not multi-device.  
     * Set autoconnect to 1.  priority and stacksize can be 0, which will use defaults. */
    : NDPluginFile(portName, queueSize, blockingCallbacks,
                   NDArrayPort, NDArrayAddr, 1, NDPluginFileLastParam,
                   1, -1, asynGenericPointerMask, asynGenericPointerMask, 
                   ASYN_CANBLOCK, 1, priority, stackSize)
{
    //const char *functionName = "NDFileNetCDF";
    
    this->supportsMultipleArrays = 1;
    this->pAttributeId = NULL;
}

/** Configuration routine.  Called directly, or from the iocsh function in NDFileEpics */
extern "C" int NDFileNetCDFConfigure(const char *portName, int queueSize, int blockingCallbacks, 
                                     const char *NDArrayPort, int NDArrayAddr,
                                     int priority, int stackSize)
{
    new NDFileNetCDF(portName, queueSize, blockingCallbacks, NDArrayPort, NDArrayAddr,
                     priority, stackSize);
    return(asynSuccess);
}


/** EPICS iocsh shell commands */
static const iocshArg initArg0 = { "portName",iocshArgString};
static const iocshArg initArg1 = { "frame queue size",iocshArgInt};
static const iocshArg initArg2 = { "blocking callbacks",iocshArgInt};
static const iocshArg initArg3 = { "NDArray Port",iocshArgString};
static const iocshArg initArg4 = { "NDArray Addr",iocshArgInt};
static const iocshArg initArg5 = { "priority",iocshArgInt};
static const iocshArg initArg6 = { "stack size",iocshArgInt};
static const iocshArg * const initArgs[] = {&initArg0,
                                            &initArg1,
                                            &initArg2,
                                            &initArg3,
                                            &initArg4,
                                            &initArg5,
                                            &initArg6};
static const iocshFuncDef initFuncDef = {"NDFileNetCDFConfigure",7,initArgs};
static void initCallFunc(const iocshArgBuf *args)
{
    NDFileNetCDFConfigure(args[0].sval, args[1].ival, args[2].ival, args[3].sval, 
                             args[4].ival, args[5].ival, args[6].ival);
}

extern "C" void NDFileNetCDFRegister(void)
{
    iocshRegister(&initFuncDef,initCallFunc);
}

epicsExportRegistrar(NDFileNetCDFRegister);
