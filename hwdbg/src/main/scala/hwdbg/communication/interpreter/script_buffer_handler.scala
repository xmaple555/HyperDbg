/**
 * @file
 *   script_buffer_handler.scala
 * @author
 *   Sina Karvandi (sina@hyperdbg.org)
 * @brief
 *   Configures the script stages from shared memory
 * @details
 * @version 0.1
 * @date
 *   2024-06-14
 *
 * @copyright
 *   This project is released under the GNU Public License v3.
 */
package hwdbg.communication.interpreter

import chisel3._
import chisel3.util.{switch, is}
import circt.stage.ChiselStage

import hwdbg.configs._
import hwdbg.types._
import hwdbg.script._

object InterpreterScriptBufferHandlerEnums {
  object State extends ChiselEnum {
    val sIdle, sReadSizeOfBuffer, sReadTypeOfOperator, sReadValueOfOperator, sDone = Value
  }
}

class InterpreterScriptBufferHandler(
    debug: Boolean = DebuggerConfigurations.ENABLE_DEBUG,
    instanceInfo: HwdbgInstanceInformation,
    bramDataWidth: Int
) extends Module {

  //
  // Import state enum
  //
  import InterpreterScriptBufferHandlerEnums.State
  import InterpreterScriptBufferHandlerEnums.State._

  val io = IO(new Bundle {

    //
    // Chip signals
    //
    val en = Input(Bool()) // chip enable signal

    //
    // Receiving signals
    //
    val readNextData = Output(Bool()) // whether the next data should be read or not?

    val dataValidInput = Input(Bool()) // whether data on the receiving data line is valid or not?
    val receivingData = Input(UInt(bramDataWidth.W)) // data to be received in interpreter

    //
    // Script stage configuration signals
    //
    val finishedConfiguration = Output(Bool()) // whether configuration finished or not?
    val moveToNextStage = Output(Bool()) // whether configuration finished configuring the current stage or not?
    val configureStage = Output(Bool()) // whether the configuration of stage should start or not?
    val targetOperator = Output(new HwdbgShortSymbol(instanceInfo.scriptVariableLength)) // Current operator to be configured
  })

  //
  // State registers
  //
  val state = RegInit(sIdle)

  //
  // Internal registers
  //
  val regScriptNumberOfSymbols = Reg(UInt(bramDataWidth.W))

  //
  // Output pins
  //
  val readNextData = WireInit(false.B)

  val finishedConfiguration = WireInit(false.B)
  val configureStage = WireInit(false.B)
  val moveToNextStage = WireInit(false.B)

  val regTargetOperator = Reg(new HwdbgShortSymbol(instanceInfo.scriptVariableLength))

  //
  // Apply the chip enable signal
  //
  when(io.en === true.B) {

    switch(state) {

      is(sIdle) { 

        //
        // Read next data for the size of the buffer
        //
        readNextData := true.B

        //
        // Move to the next state
        //
        state := sReadSizeOfBuffer
      }
      is(sReadSizeOfBuffer) { 

        when(io.dataValidInput) {

            //
            // Data is valid and number of symbols is now available
            //
            regScriptNumberOfSymbols := io.receivingData

            //
            // Request next data
            //
            readNextData := true.B

            //
            // Move to the configuration state
            //
            state := sReadTypeOfOperator

        }.otherwise{

            //
            // Stay at the same state since the data is not yet received
            //
            state := sReadSizeOfBuffer
        }
      }
      is(sReadTypeOfOperator) {


        when(io.dataValidInput) {

            //
            // Configure the stages
            //
            configureStage := true.B

            //
            // Request next data
            //
            readNextData := true.B

            //
            // Read the operator's "Type" data
            //
            regTargetOperator.Type := io.receivingData

            //
            // Next, we need to read "Value"
            //
            state := sReadValueOfOperator

        } .otherwise {

            //
            // Stay at the same state since the data is not received yet
            //
            state := sReadTypeOfOperator
        }
       }
      is(sReadValueOfOperator) {

        when(io.dataValidInput) {

            //
            // Configure the stages
            //
            configureStage := true.B

            //
            // Read the operator's "Value" data
            //
            regTargetOperator.Value := io.receivingData

            //
            // Check if reading is scripts are finished or not
            //
            when (regScriptNumberOfSymbols === 0.U) {
                
                //
                // Configurartion was done
                //
                state := sDone
            }.otherwise {

            //
            // Request next data
            //
            readNextData := true.B

            //
            // Again, read the next type
            //
            state := sReadTypeOfOperator

            }
        } .otherwise {

            //
            // Stay at the same state since the data is not received yet
            //
            state := sReadValueOfOperator
        }
      }
      is(sDone) {

        //
        // Finished configuration
        //
        finishedConfiguration := true.B

        //
        // Move to the idle state
        //
        state := sIdle
      }
     }
    }

  //
  // Connect output pins
  //
  io.readNextData := readNextData

  io.finishedConfiguration := finishedConfiguration
  io.configureStage := configureStage
  io.moveToNextStage := moveToNextStage
  io.targetOperator := regTargetOperator

}

object InterpreterScriptBufferHandler {

  def apply(
    debug: Boolean = DebuggerConfigurations.ENABLE_DEBUG,
    instanceInfo: HwdbgInstanceInformation,
    bramDataWidth: Int
  )(
      en: Bool,
      dataValidInput: Bool,
      receivingData: UInt
  ): (Bool, Bool, Bool, Bool, HwdbgShortSymbol) = {

    val interpreterScriptBufferHandler = Module(
      new InterpreterScriptBufferHandler(
        debug,
        instanceInfo,
        bramDataWidth
      )
    )

    val readNextData = Wire(Bool())

    val finishedConfiguration = Wire(Bool())
    val configureStage = Wire(Bool())
    val moveToNextStage = Wire(Bool())
    val targetOperator = Wire(new HwdbgShortSymbol(instanceInfo.scriptVariableLength))

    //
    // Configure the input signals
    //
    interpreterScriptBufferHandler.io.en := en

    //
    // Configure the input signals related to the receiving signals
    //
    interpreterScriptBufferHandler.io.dataValidInput := dataValidInput
    interpreterScriptBufferHandler.io.receivingData := receivingData

    //
    // Configure the output signals
    //
    readNextData := interpreterScriptBufferHandler.io.readNextData

    //
    // Configure the output signals related to configuring stage operators
    //
    finishedConfiguration := interpreterScriptBufferHandler.io.finishedConfiguration
    configureStage := interpreterScriptBufferHandler.io.configureStage
    moveToNextStage := interpreterScriptBufferHandler.io.moveToNextStage
    targetOperator := interpreterScriptBufferHandler.io.targetOperator

    //
    // Return the output result
    //
    (
      readNextData,
      finishedConfiguration,
      configureStage,
      moveToNextStage,
      targetOperator
    )
  }
}
